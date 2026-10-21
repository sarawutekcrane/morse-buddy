#!/usr/bin/env python3
"""Executable host model of the Race R3-R5 room protocol (design verification only).

Not firmware. Models only what the counterexamples need: coordinator request
processing, room-identity allocation, member room-state acceptance, identity
verification, member record recovery (with NVS write failure) and JOIN
completion. Message delivery order is scripted per scenario.

Rule sets:
  r6 -- design revision 6 as written
  f1 -- round-1 corrections (nonce identity, persisted appliedV, terminal JOIN)
  f2 -- round-2 corrections (identity mismatch -> verify with the coordinator;
        apply only after the recovery floor is durable)
  f3 -- round-3 correction: bounded QUERY lifecycle (binding, invalidation,
        retry by replacement) on top of f2; round 4 aligns it with the design
        text: durable pending requests survive reboot and are resent
        identically, a reboot restores the snapshot from an RS at exactly
        appliedV, and a running member treats an equal version with different
        content as a conflict (freeze)
  m4 -- the round-4 MODEL's f3 behaviour (for the round-5 regressions only):
        persists appliedV even when a reboot recovery receives exactly
        appliedV, and keeps applying packets after a conflict freeze
        (f3 now: no write at the equal version; frozen = Leave only)
  r7txt -- revision 7's TEXT as written (for the round-4 regressions only):
        f3, except that reboot also clears the pending request (6.2) and an
        equal-version RS after reboot is a 'duplicate' that restores nothing (6.4)

Usage:  python3 -I race_model.py
Exit 0 iff every scenario's outcome under every rule set equals its expected
outcome (EXPECT table): each counterexample fails under the rule sets it was
found against and passes under f3; the normal-reboot control passes everywhere.
"""
import sys

RULES = """
f1 (round 1):
 F1 room identity = (C, n, nonce32); requests/replies/RS carry it; coordinator
    answers any other (n, nonce) with UNKNOWN_ROOM. Member: an RS on r2/<C> with a
    different (n, nonce) -> higher n superseded / equal n different nonce terminal /
    lower n stale.
 F2 rm_member stores appliedV; after reboot accept only version >= appliedV;
    if persisting appliedV fails the RS is still applied in RAM.
 F3 a JOIN that ends without admission is terminal; no automatic reissue.

f2 (round 2) = f1 with F1 and F2 replaced:
 G1 identity mismatch never changes member state by itself. A nonce
    distinguishes identities but does not order them, and n does not order
    rooms across an identity loss either. An RS on r2/<C> whose (n, nonce)
    differs from the record -> 'Verifying room...' and a QUERY(seq) to C
    (rate-limited). C answers every QUERY with REPLY carrying its CURRENT room
    identity (n, nonce, version) or NONE. The member accepts the reply only if
    target, coordinator and seq match its outstanding query, then:
      reply identity == record  -> the mismatched RS was foreign/stale: ignore it
      reply identity != record, or NONE -> my room is gone: terminal
      no reply (C unavailable)  -> stay 'Verifying', no state change
    A genuine replacement is therefore confirmed only by the coordinator's own
    answer about its current room, never by a snapshot alone.
 G2 apply-after-durable: a member applies (displays, completes pending ops,
    acts on) RS version v only after persisting appliedV = v succeeded. If the
    write fails, nothing is applied: the confirmed state stays at the durable
    floor, the UI shows 'Storage error - room paused', and the write is retried
    on the next RS (or periodically). So the confirmed version can never exceed
    the durable floor, and recovery can never roll it back.
 F3 unchanged.

f3 (round 3) = f2 plus H1 (QUERY lifecycle):
 H1 at most one outstanding verification, bound to (group, coordinator, n,
    nonce, attempt) where 'attempt' is the seq of the request that created the
    current membership record (JOIN, or room creation). It is invalidated
    (cleared) on: Leave (any), terminal transition, Join / new-room selection,
    and reboot. A reply is accepted only if group, coordinator, target and qseq
    match the outstanding query AND the query's binding still equals the
    current record. Retry: if no matching reply within the retry interval, a
    NEW query with a fresh durable qseq REPLACES the outstanding one (the old
    qseq stops matching) -- never two at once. Replies are therefore usable
    only for the exact verification that is still outstanding.
"""

# ----------------------------------------------------------------------------
class RS:
    def __init__(self, C, n, nonce, v, entries, floor=0, phase="LOBBY", roundStart=0, secret=None):
        self.C, self.n, self.nonce, self.v = C, n, nonce, v
        self.entries = dict(entries)
        self.floor, self.phase, self.roundStart, self.secret = floor, phase, roundStart, secret
    def copy(self):
        return RS(self.C, self.n, self.nonce, self.v, {k: dict(e) for k, e in self.entries.items()},
                  self.floor, self.phase, self.roundStart, self.secret)
    def active(self):
        return sorted(k for k, e in self.entries.items() if e["active"])
    def ident(self):
        return (self.n, self.nonce)

class Req:
    def __init__(self, op, C, n, nonce, sender, seq, basis, roundStart=0, guess=None):
        self.op, self.C, self.n, self.nonce, self.sender, self.seq, self.basis = op, C, n, nonce, sender, seq, basis
        self.roundStart, self.guess = roundStart, guess

class Reply:
    def __init__(self, C, target, seq, code, cur, group="G1"):
        self.C, self.target, self.seq, self.code, self.cur = C, target, seq, code, cur  # cur = (n, nonce, v) or None
        self.group = group  # the group connection the reply arrived on

def uses_nonce(rules):
    return rules in ("f1", "f2", "f3", "r7txt", "m4")
def verifies(rules):
    return rules in ("f2", "f3", "r7txt", "m4")
def final_like(rules):
    return rules in ("f3", "r7txt", "m4")
def round5(rules):
    """round-5 alignment (no equal-version write; enforced freeze) -- not in m4"""
    return rules in ("f3", "r7txt")

# ----------------------------------------------------------------------------
class Coordinator:
    CAP = 20
    def __init__(self, rules, C):
        self.rules, self.C = rules, C
        self.rs = None
        self.id_counter = 0
        self.last_room_counter = 0
        self.log = []
    def create_room(self, nonce, id_counter_after_loss=None):
        if id_counter_after_loss is not None:
            self.id_counter = id_counter_after_loss
        self.id_counter += 1
        n = max(self.last_room_counter + 1, self.id_counter)
        self.last_room_counter = n
        self.rs = RS(self.C, n, nonce if uses_nonce(self.rules) else 0, 1,
                     {self.C: {"active": True, "lastSeq": 0, "score": 0}})
        self.log.append(f"create room (n={n}, nonce={self.rs.nonce:#x}) v1")
        return self.rs.copy()
    def lose_identity(self):
        self.rs = None
        self.id_counter = 0
        self.last_room_counter = 0
        self.log.append("IDENTITY STORAGE LOST")
    def commit(self):
        self.rs.v += 1
        return self.rs.copy()
    def query(self, q):
        """QUERY: always answered with the CURRENT room identity (f2)."""
        cur = (self.rs.n, self.rs.nonce, self.rs.v) if self.rs and self.rs.phase != "CLOSED" else None
        return Reply(self.C, q.sender, q.seq, "CURRENT", cur)
    def handle(self, q):
        rs = self.rs
        same = (q.n == rs.n) and (not uses_nonce(self.rules) or q.nonce == rs.nonce)
        if not same:
            self.log.append(f"{q.op} from {q.sender} seq{q.seq}: UNKNOWN_ROOM")
            return ("UNKNOWN_ROOM", None)
        e = rs.entries.get(q.sender)
        if e is not None and q.seq <= e["lastSeq"]:
            return ("OK(dup)", None)
        if q.op == "JOIN":
            if e is None and q.basis < rs.floor:
                self.log.append(f"JOIN {q.sender} seq{q.seq}: STALE_BASIS")
                return ("STALE_BASIS", None)
            if len(rs.active()) >= self.CAP and not (e and e["active"]):
                if e is None:
                    rs.floor = rs.v + 1
                else:
                    e["lastSeq"] = q.seq
                snap = self.commit()
                self.log.append(f"JOIN {q.sender} seq{q.seq}: FULL (commit v{snap.v}, floor={snap.floor})")
                return ("FULL", snap)
            rs.entries.setdefault(q.sender, {"active": True, "lastSeq": 0, "score": 0})
            rs.entries[q.sender].update(active=True, lastSeq=q.seq)
            snap = self.commit()
            self.log.append(f"JOIN {q.sender} seq{q.seq}: ACCEPTED -> v{snap.v} active={snap.active()}")
            return ("OK", snap)
        if q.op == "LEAVE":
            if e is None:
                rs.floor = rs.v + 1
            else:
                e.update(active=False, lastSeq=q.seq)
            snap = self.commit()
            self.log.append(f"LEAVE {q.sender} seq{q.seq}: -> v{snap.v}")
            return ("OK", snap)
        if q.op == "CLAIM":
            if e is None or not e["active"]:
                self.log.append(f"CLAIM {q.sender} seq{q.seq}: NOT_MEMBER")
                return ("NOT_MEMBER", None)
            if rs.phase != "ROUND" or q.roundStart != rs.roundStart:
                return ("STALE_ROUND", None)
            if q.guess != rs.secret:
                return ("BAD_GUESS", None)
            e["score"] += 1; e["lastSeq"] = q.seq; rs.phase = "LOBBY"
            snap = self.commit()
            self.log.append(f"CLAIM {q.sender} seq{q.seq}: WIN -> v{snap.v}")
            return ("OK", snap)
        raise ValueError(q.op)
    def observe_own_slot(self, rs):
        if self.rules != "r6" or self.rs is None:
            return "ignored"
        if rs.n > self.rs.n or (rs.n == self.rs.n and rs.v > self.rs.v):
            self.log.append(f"ID CONFLICT detected late (observed v{rs.v} > own v{self.rs.v}); room closed")
            self.rs.phase = "CLOSED"
            return "conflict"
        self.log.append(f"observed own slot v{rs.v} <= own v{self.rs.v}: treated as own stale copy")
        return "stale-own"

# ----------------------------------------------------------------------------
def _content(rs):
    return (rs.C, rs.n, rs.nonce, rs.v, tuple(sorted((k, tuple(sorted(e.items()))) for k, e in rs.entries.items())),
            rs.floor, rs.phase, rs.roundStart, rs.secret)

class Member:
    def __init__(self, rules, me):
        self.rules, self.me = rules, me
        self.rec = None        # durable rm_member (dict); 'appliedV' is the durable floor
        self.applied = None    # RAM: the confirmed/displayed state
        self.ui = ""
        self.sent = []
        self.seq = 100
        self.nvs_ok = True     # fault injection for rm_member writes
        self.query_seq = None  # outstanding identity QUERY (f2/f3)
        self.query_bind = None # f3: (group, C, n, nonce, attempt) of the outstanding QUERY
        self.group = "G1"
        self.queries_sent = 0
        self.shown = []        # every version ever displayed as confirmed (rollback check)
        self.frozen = False
        self.nvs_writes_attempted = 0
    # -- durable write of the recovery floor ---------------------------------
    def _persist_applied(self, v):
        self.nvs_writes_attempted += 1
        if not self.nvs_ok:
            return False
        self.rec["appliedV"] = v
        return True
    def _show(self, rs):
        self.applied = rs.copy()
        self.shown.append(rs.v)
    def _binding(self):
        r = self.rec
        return (self.group, r["C"], r["n"], r["nonce"], r.get("attempt"))
    def _invalidate_query(self):
        if final_like(self.rules):
            self.query_seq, self.query_bind = None, None
    def record_room(self, rs, state="MEMBER", attempt=None):
        self._invalidate_query()
        self.seq += 1
        self.rec = {"C": rs.C, "n": rs.n, "nonce": rs.nonce, "state": state, "pending": None, "appliedV": rs.v,
                    "attempt": attempt if attempt is not None else self.seq}
        self._show(rs)
    def leave(self):
        """Explicit Leave (modelled as acknowledged): record -> NONE."""
        self.rec["state"] = "NONE"
        self.ui = "Left"
        self._invalidate_query()
    def _send_query(self):
        self.seq += 1
        self.query_seq = self.seq
        self.query_bind = self._binding()
        self.queries_sent += 1
        self.sent.append(Req("QUERY", self.rec["C"], self.rec["n"], self.rec["nonce"], self.me, self.seq, 0))
    def query_timeout(self):
        """Retry interval elapsed without a matching reply."""
        if final_like(self.rules) and self.query_seq is not None:
            self._send_query()          # replaces the outstanding query (fresh qseq)
    def reboot(self):
        self.applied = None
        self.query_seq = None          # outstanding verification is always invalidated on reboot
        self.query_bind = None
        if self.rules == "r7txt" and self.rec is not None:   # revision 7 text: pending seq cleared too (defect)
            self.rec["pending"] = None
            self.rec["pending_req"] = None
        if uses_nonce(self.rules):
            self.ui = "Reconnecting..."
    def can_act(self, action):
        """Gameplay actions are refused while frozen; Leave is always allowed."""
        return action == "leave" or not (self.frozen and round5(self.rules))
    def on_rs(self, rs):
        r = self.rec
        if r is None or rs.C != r["C"] or r["state"] == "NONE":
            return "not-mine"
        if self.frozen and round5(self.rules):
            return "ignored (frozen: Leave only)"
        if uses_nonce(self.rules) and rs.ident() != (r["n"], r["nonce"]):
            if self.rules == "f1":
                if rs.n > r["n"]:
                    self.ui = "Room closed (superseded)"; r["state"] = "NONE"; return "terminal"
                if rs.n == r["n"]:
                    self.ui = "Room closed (room identity changed)"; r["state"] = "NONE"; return "terminal"
                return "stale"
            # f2 / G1: never decide from a snapshot; ask the coordinator
            if self.query_seq is None:
                self._send_query()
            self.ui = "Verifying room..."
            return "verify"
        if self.rules == "r6" and rs.n != r["n"]:
            if rs.n < r["n"]:
                return "stale"
            r["state"], self.ui = "NONE", "Room closed (superseded)"   # revision 6: higher n supersedes
            return "terminal"
        floor_v = self.applied.v if self.applied else (r["appliedV"] if uses_nonce(self.rules) else 0)
        if rs.v < floor_v:
            return "stale"
        if self.applied is not None and rs.v == self.applied.v and final_like(self.rules):
            if _content(rs) == _content(self.applied):
                return "duplicate"
            self.ui = "Room data inconsistent"
            self.frozen = True
            return "conflict"
        if self.applied is None and rs.v == floor_v and self.rules == "r7txt":
            return "duplicate (nothing restored)"     # revision 7 text: equal version after reboot (defect)
        restore_at_floor = self.applied is None and rs.v == floor_v and round5(self.rules)
        if self.rules in ("f2", "f3", "r7txt", "m4") and not restore_at_floor:   # equal version: floor already durable
            if not self._persist_applied(rs.v):            # G2: apply only after the floor is durable
                self.ui = "Storage error - room paused"
                return "not-applied (storage)"
        elif self.rules == "f1":
            self._persist_applied(rs.v)                     # F2: failure ignored, RAM still applied
        self._show(rs)
        if self.ui in ("Reconnecting...", "Verifying room...", "Storage error - room paused"):
            self.ui = ""
        self._complete_from_state()
        return "applied"
    def on_reply(self, rep):
        """f2 / G1 (+ f3 / H1): identity verification answer."""
        r = self.rec
        if (rep.C != r["C"] or rep.target != self.me or self.query_seq is None or rep.seq != self.query_seq):
            return "ignored (not my outstanding query)"
        if final_like(self.rules) and (rep.group != self.group or self.query_bind != self._binding()):
            return "ignored (binding/group mismatch)"
        self.query_seq, self.query_bind = None, None
        if rep.cur is not None and rep.cur[:2] == (r["n"], r["nonce"]):
            self.ui = ""
            return "confirmed mine (mismatched snapshot was foreign/stale)"
        r["state"] = "NONE"
        self.ui = "Room closed (room identity changed)"
        self._invalidate_query()
        return "terminal (coordinator's current room is not mine)"
    def send_request(self, q):
        """Durable pending request: written to rm_member before it is sent."""
        self.rec["pending_req"] = q
        self.sent.append(q)
        return q
    def resume_after_reboot(self):
        """Resend the durable pending request with identical bytes (same seq)."""
        q = self.rec.get("pending_req") if self.rec else None
        if q is not None:
            self.sent.append(q)
        return q
    def outstanding_query(self):
        return next((q for q in reversed(self.sent) if q.op == "QUERY" and q.seq == self.query_seq), None)
    def join(self, rs_seen):
        self._invalidate_query()
        self.seq += 1
        self.rec = {"C": rs_seen.C, "n": rs_seen.n, "nonce": rs_seen.nonce, "state": "JOINING",
                    "pending": ("JOIN", self.seq, rs_seen.v), "appliedV": rs_seen.v, "attempt": self.seq}
        self._show(rs_seen)
        q = Req("JOIN", rs_seen.C, rs_seen.n, rs_seen.nonce, self.me, self.seq, rs_seen.v)
        self.sent.append(q)
        return q
    def _complete_from_state(self):
        r, rs = self.rec, self.applied
        if not r or not r["pending"] or r["pending"][0] != "JOIN":
            return
        _, seq, basis = r["pending"]
        e = rs.entries.get(self.me)
        if e and e["active"] and e["lastSeq"] >= seq:
            r["state"], r["pending"], self.ui = "MEMBER", None, "Joined"
            return
        if e is None and rs.floor > basis:
            if self.rules == "r6":
                if len(rs.active()) >= Coordinator.CAP:
                    r["state"], r["pending"], self.ui = "NONE", None, "Room full"
                else:
                    self.ui = "reissuing JOIN (auto)"
                    self.seq += 1
                    r["pending"] = ("JOIN", self.seq, rs.v)
                    self.sent.append(Req("JOIN", rs.C, rs.n, rs.nonce, self.me, self.seq, rs.v))
            else:
                r["state"], r["pending"], self.ui = "NONE", None, "Could not join - try again"

def verify_if_asked(M, C, t, deliver=True):
    """Harness: if M sent an identity QUERY, deliver C's answer (unless C is unavailable)."""
    q = M.outstanding_query()
    if q is None:
        return None
    if not deliver:
        t.append("  QUERY sent; coordinator unavailable -> no reply")
        return None
    res = M.on_reply(C.query(q))
    t.append(f"  QUERY seq{q.seq} -> C answers its current room -> {res}")
    return res

# ----------------------------------------------------------------------------
# Original scenarios (round 1)
def scenario_identity_reuse(rules):
    t, ok = [], True
    C = Coordinator(rules, "C")
    C.id_counter = 39
    old = C.create_room(nonce=0xA1)
    for i in range(14):
        C.commit()
    C.rs.entries["M"] = {"active": True, "lastSeq": 7, "score": 3}
    C.rs.phase, C.rs.roundStart, C.rs.secret = "ROUND", 12, "3047"
    old_retained = C.commit()
    t.append(f"old room (C,{old_retained.n}) v{old_retained.v}, members {old_retained.active()}")
    late_join = Req("JOIN", "C", 40, old.nonce, "D", 55, 15)
    late_claim = Req("CLAIM", "C", 40, old.nonce, "M", 9, 15, 12, "3047")
    C.lose_identity()
    new = C.create_room(nonce=0xB2, id_counter_after_loss=39)
    t.append(f"after identity loss: new room (C,{new.n}) v{new.v} nonce={new.nonce:#x}")
    C.rs.entries["B"] = {"active": True, "lastSeq": 3, "score": 0}; C.commit()
    code, _ = C.handle(late_join)
    t.append(f"old JOIN from D -> {code}")
    if code == "OK" and "D" in C.rs.active():
        ok = False; t.append("  VIOLATION: a request for the OLD room changed the NEW room (D admitted)")
    C.rs.phase, C.rs.roundStart, C.rs.secret = "ROUND", 12, "3047"  # forced coincidence (see report)
    C.rs.entries.setdefault("M", {"active": True, "lastSeq": 2, "score": 0})
    code, _ = C.handle(late_claim)
    t.append(f"old CLAIM from M -> {code}")
    if code == "OK":
        ok = False; t.append("  VIOLATION: an OLD-room claim scored in the NEW room")
    res = C.observe_own_slot(old_retained)
    t.append(f"old retained (C,40) v{old_retained.v} reaches C -> {res}")
    if rules == "r6" and res == "conflict" and not ok:
        t.append("  NOTE: detection came after the effects above; it did not prevent them")
    return ok, t

def scenario_same_room_lower_snapshot(rules):
    """1b: M is in the OLD room; the NEW room's snapshot arrives (old-member direction)."""
    t, ok = [], True
    C = Coordinator(rules, "C")
    C.id_counter = 39
    C.create_room(nonce=0xA1)
    for i in range(14): C.commit()
    C.rs.entries["M"] = {"active": True, "lastSeq": 7, "score": 5}
    old_rs = C.commit()
    M = Member(rules, "M"); M.record_room(old_rs)
    C.lose_identity()
    C.create_room(nonce=0xB2, id_counter_after_loss=39)
    for i in range(24): C.commit()
    new_rs = C.rs.copy()
    res = C.observe_own_slot(old_rs)
    t.append(f"old (C,40) v16 reaches C (own v{C.rs.v}) -> {res}")
    if rules == "r6" and res == "stale-own":
        ok = False; t.append("  VIOLATION: collision not detected; C treats a foreign old room as its own stale copy")
    r = M.on_rs(new_rs)
    t.append(f"old member M (applied old v16) receives new (C,40) v{new_rs.v} -> {r}")
    verify_if_asked(M, C, t)
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}', applied v{M.applied.v}")
    if r == "applied":
        ok = False; t.append("  VIOLATION: M adopted the NEW room's snapshot as its old room")
    elif M.rec["state"] != "NONE":
        ok = False; t.append("  VIOLATION: M's genuinely replaced room was not ended")
    return ok, t

def scenario_member_reboot_stale(rules):
    t, ok = [], True
    base = RS("C", 7, 0xC3, 18, {"C": {"active": True, "lastSeq": 0, "score": 1},
                                  "M": {"active": True, "lastSeq": 4, "score": 0}})
    v18 = base.copy()
    v20 = base.copy(); v20.v = 20; v20.entries["M"]["score"] = 2
    v21 = base.copy(); v21.v = 21; v21.entries["M"]["score"] = 2
    M = Member(rules, "M"); M.record_room(v20)
    t.append("M applied v20 (its score 2); reboot")
    M.reboot()
    r = M.on_rs(v18)
    t.append(f"stale retained v18 first -> {r}; ui='{M.ui}'")
    if r == "applied":
        ok = False; t.append("  VIOLATION: stale v18 treated as current (applied version went 20 -> 18)")
    r = M.on_rs(v21)
    t.append(f"current v21 -> {r}; applied v{M.applied.v}")
    return ok, t

def scenario_full_reply_lost(rules):
    t, ok = [], True
    C = Coordinator(rules, "C")
    C.create_room(nonce=0xD4)
    for i in range(19):
        C.rs.entries[f"P{i}"] = {"active": True, "lastSeq": 1, "score": 0}
    seen = C.commit()
    R = Member(rules, "R")
    q = R.join(seen)
    code, _ = C.handle(q)
    t.append(f"R's JOIN (basis v{q.basis}) -> {code}; the reply is LOST")
    code2, later = C.handle(Req("LEAVE", "C", C.rs.n, C.rs.nonce, "P3", 2, C.rs.v))
    t.append(f"P3 leaves -> v{later.v}, active={len(later.active())}")
    R.on_rs(later)
    t.append(f"R receives only v{later.v} -> ui='{R.ui}', state={R.rec['state']}")
    for x in [x for x in R.sent[1:] if x.op == "JOIN"]:
        c, _ = C.handle(x)
        t.append(f"  auto-reissued JOIN seq{x.seq} -> {c}")
        if c == "OK":
            ok = False; t.append("  VIOLATION: a terminal FULL turned into an automatic admission")
    return ok, t

def scenario_normal_reboot(rules):
    t = []
    C = Coordinator(rules, "C")
    C.create_room(nonce=0xE5)
    C.commit()
    M = Member(rules, "M"); M.record_room(C.rs.copy())
    saved = (C.rs.copy(), C.id_counter, C.last_room_counter)
    C.rs, C.id_counter, C.last_room_counter = saved
    snap = C.commit()
    r = M.on_rs(snap)
    t.append(f"coordinator rebooted with intact records; next v{snap.v} -> member {r}")
    return r == "applied", t

# ----------------------------------------------------------------------------
# Round-2 scenarios
def _two_rooms(rules, old_n_after_loss=39):
    """C's old room (nonce A) at v16; identity loss; new room (nonce B) at v25."""
    C = Coordinator(rules, "C")
    C.id_counter = 39
    C.create_room(nonce=0xA1)
    for i in range(14): C.commit()
    C.rs.entries["M"] = {"active": True, "lastSeq": 7, "score": 5}
    old_rs = C.commit()
    C.lose_identity()
    C.create_room(nonce=0xB2, id_counter_after_loss=old_n_after_loss)
    C.rs.entries["M"] = {"active": True, "lastSeq": 3, "score": 1}
    for i in range(23): C.commit()
    return C, old_rs, C.rs.copy()

def scenario_reverse_old_snapshot(rules):
    """4a: M is in the NEW room (applied v25); a delayed OLD snapshot (same n, other nonce) arrives,
    plus a delayed stale reply to an earlier query."""
    t, ok = [], True
    C, old_rs, new_rs = _two_rooms(rules)
    M = Member(rules, "M"); M.record_room(new_rs)
    t.append(f"M in NEW room (C,{new_rs.n},nonce {new_rs.nonce:#x}) applied v{new_rs.v}")
    r = M.on_rs(old_rs)
    t.append(f"delayed OLD snapshot (C,{old_rs.n},nonce {old_rs.nonce:#x}) v{old_rs.v} -> {r}")
    stale_reply = Reply("C", "M", 1, "CURRENT", (old_rs.n, old_rs.nonce, old_rs.v))
    t.append(f"  a delayed reply to an earlier query seq1 arrives -> {M.on_reply(stale_reply) if M.rec['state'] != 'NONE' else 'n/a'}")
    verify_if_asked(M, C, t)
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}'")
    if M.rec["state"] == "NONE":
        ok = False; t.append("  VIOLATION: an OLD snapshot closed the NEW room for M")
    return ok, t

def scenario_reverse_higher_n(rules):
    """4b: identity loss gave the new room a LOWER n; M is in the new room; an old snapshot with higher n arrives."""
    t, ok = [], True
    C, old_rs, new_rs = _two_rooms(rules, old_n_after_loss=0)   # new room n=1, old room n=40
    M = Member(rules, "M"); M.record_room(new_rs)
    t.append(f"M in NEW room (C,{new_rs.n}) applied v{new_rs.v}; old snapshot (C,{old_rs.n}) v{old_rs.v} arrives")
    r = M.on_rs(old_rs)
    t.append(f"  -> {r}")
    verify_if_asked(M, C, t)
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}'")
    if M.rec["state"] == "NONE":
        ok = False; t.append("  VIOLATION: a higher n from before the identity loss 'superseded' the current room")
    return ok, t

def scenario_genuine_supersession(rules):
    """4c: normal life: C closed room 40 and created room 41; M (still recorded in 40) sees 41."""
    t, ok = [], True
    C = Coordinator(rules, "C")
    C.id_counter = 39
    r40 = C.create_room(nonce=0xA1)
    M = Member(rules, "M"); M.record_room(r40)
    C.rs.phase = "CLOSED"
    r41 = C.create_room(nonce=0xC7)
    r = M.on_rs(r41)
    t.append(f"M (room 40) receives (C,41) v{r41.v} -> {r}")
    verify_if_asked(M, C, t)
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}'")
    if M.rec["state"] != "NONE":
        ok = False; t.append("  VIOLATION: a genuine replacement was not recognised")
    return ok, t

def scenario_verify_coordinator_offline(rules):
    """4d: mismatched snapshot while C cannot answer: no state change either way."""
    t, ok = [], True
    C, old_rs, new_rs = _two_rooms(rules)
    M = Member(rules, "M"); M.record_room(new_rs)
    r = M.on_rs(old_rs)
    t.append(f"M (new room) receives old snapshot -> {r}")
    verify_if_asked(M, C, t, deliver=False)
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}'")
    if M.rec["state"] == "NONE":
        ok = False; t.append("  VIOLATION: room ended without the coordinator's confirmation")
    return ok, t

def scenario_persist_failure(rules):
    """5: appliedV write fails while v20 arrives; reboot; v18 delivered."""
    t, ok = [], True
    base = RS("C", 7, 0xC3, 18, {"C": {"active": True, "lastSeq": 0, "score": 1},
                                  "M": {"active": True, "lastSeq": 4, "score": 0}})
    v18 = base.copy()
    v20 = base.copy(); v20.v = 20; v20.entries["M"]["score"] = 2
    M = Member(rules, "M"); M.record_room(v18)
    M.nvs_ok = False
    r = M.on_rs(v20)
    t.append(f"v20 arrives while the rm_member write FAILS -> {r}; shown v{M.applied.v}; durable floor v{M.rec['appliedV']}; ui='{M.ui}'")
    M.nvs_ok = True
    M.reboot()
    r = M.on_rs(v18)
    t.append(f"reboot; retained v18 delivered -> {r}; shown history {M.shown}")
    if any(b < a for a, b in zip(M.shown, M.shown[1:])):
        ok = False; t.append("  VIOLATION: the confirmed/displayed state went backwards (20 -> 18) after recovery")
    return ok, t

def scenario_stale_query_reply_after_rejoin(rules):
    """6: QUERY in room A; reply delayed; user leaves A and joins B (same coordinator); old reply arrives."""
    t, ok = [], True
    C = Coordinator(rules, "C")
    C.id_counter = 39
    roomA = C.create_room(nonce=0xA1)
    C.rs.entries["M"] = {"active": True, "lastSeq": 7, "score": 2}
    a = C.commit()
    M = Member(rules, "M"); M.record_room(a)
    foreign = RS("C", 40, 0x99, 3, {})          # some mismatched snapshot (e.g. a stale copy)
    if not uses_nonce(rules):
        foreign = RS("C", 41, 0, 3, {})
    r = M.on_rs(foreign)
    q = M.outstanding_query()
    t.append(f"M in A (C,{a.n},{a.nonce:#x}); mismatched snapshot -> {r}; QUERY {'seq'+str(q.seq) if q else 'none'}")
    delayed = C.query(q) if q else None          # C answers while its current room is A; reply DELAYED
    M.leave()
    C.rs.phase = "CLOSED"
    roomB = C.create_room(nonce=0xB7)
    C.rs.entries["M"] = {"active": True, "lastSeq": 1, "score": 0}
    b = C.commit()
    M.record_room(b)
    t.append(f"M leaves A; C closes A and creates B (C,{b.n},{b.nonce:#x}); M joins B -> state {M.rec['state']}")
    if delayed:
        res = M.on_reply(delayed)
        t.append(f"delayed reply to the A-era QUERY arrives (says current = A) -> {res}")
    t.append(f"  M: state={M.rec['state']}, ui='{M.ui}'")
    if M.rec["state"] != "MEMBER":
        ok = False; t.append("  VIOLATION: a reply to a verification from the previous membership ended the new room")
    return ok, t

def scenario_lost_query_recovery(rules):
    """7: the first QUERY is lost. After the retry interval the coordinator answers every QUERY it
    actually receives. Verification must complete; exactly one query is outstanding at a time; a
    reply arriving via another group's connection and a late copy of the first query's reply are
    ignored."""
    t, ok = [], True
    C, old_rs, new_rs = _two_rooms(rules)
    M = Member(rules, "M"); M.record_room(new_rs)
    M.on_rs(old_rs)
    q1 = M.outstanding_query()
    t.append(f"M (new room) sees an old snapshot -> QUERY {'seq'+str(q1.seq) if q1 else 'none'}; it is LOST")
    late_copy_of_q1_reply = C.query(q1) if q1 else None    # kept for later delivery (a delayed duplicate)
    M.query_timeout()
    q2 = M.outstanding_query()
    received = [q2] if (q2 is not None and q2 is not q1) else []
    t.append(f"retry interval elapses -> outstanding {'seq'+str(q2.seq) if q2 else 'none'}; "
             f"queries sent {M.queries_sent}; C received {[x.seq for x in received]}")
    for q in received:
        foreign = C.query(q); foreign.group = "G2"
        t.append(f"reply to seq{q.seq} via group G2's connection -> {M.on_reply(foreign)}")
        t.append(f"reply to seq{q.seq} via G1 -> {M.on_reply(C.query(q))}")
    t.append(f"  after the retry window: state={M.rec['state']}, ui='{M.ui}', outstanding={M.query_seq}")
    if M.rec["state"] != "MEMBER" or M.ui == "Verifying room..." or M.query_seq is not None:
        ok = False; t.append("  VIOLATION: verification never completed after a lost QUERY")
    if late_copy_of_q1_reply is not None:
        res = M.on_reply(late_copy_of_q1_reply)
        t.append(f"late copy of the seq{q1.seq} reply -> {res}")
        if not res.startswith("ignored") and ok:
            ok = False; t.append("  VIOLATION: a stale reply matched")
    return ok, t

def scenario_pending_survives_reboot(rules):
    """8: a durable pending CLAIM and an outstanding verification QUERY exist; reboot.
    The QUERY must be invalidated; the CLAIM must survive and be resent with identical fields."""
    t, ok = [], True
    C = Coordinator(rules, "C")
    rs = C.create_room(nonce=0xF1)
    C.rs.entries["M"] = {"active": True, "lastSeq": 3, "score": 0}
    C.rs.phase, C.rs.roundStart, C.rs.secret = "ROUND", 5, "3047"
    cur = C.commit()
    M = Member(rules, "M"); M.record_room(cur)
    M.seq += 1
    claim = M.send_request(Req("CLAIM", "C", cur.n, cur.nonce, "M", M.seq, cur.v, cur.roundStart, "3047"))
    if verifies(rules):
        M.on_rs(RS("C", cur.n, 0x77, 2, {}))           # some mismatched snapshot -> outstanding QUERY
    t.append(f"before reboot: pending CLAIM seq{claim.seq} (round {claim.roundStart}, guess {claim.guess}); "
             f"outstanding QUERY {M.query_seq}")
    M.reboot()
    q = M.resume_after_reboot()
    t.append(f"after reboot: outstanding QUERY {M.query_seq}; resent "
             f"{(q.op + ' seq' + str(q.seq) + ' round ' + str(q.roundStart) + ' guess ' + str(q.guess)) if q else 'nothing'}")
    if M.query_seq is not None:
        ok = False; t.append("  VIOLATION: the verification QUERY survived the reboot")
    if q is None or (q.op, q.seq, q.roundStart, q.guess) != (claim.op, claim.seq, claim.roundStart, claim.guess):
        ok = False; t.append("  VIOLATION: the durable pending request was not resent identically after reboot")
    else:
        code, _ = C.handle(q)
        t.append(f"  coordinator processes the resent CLAIM -> {code}")
    return ok, t

def scenario_reboot_exact_version(rules):
    """9: member applied v20 (appliedV 20); reboot; the coordinator's current state is exactly v20."""
    t, ok = [], True
    base = RS("C", 7, 0xC3, 20, {"C": {"active": True, "lastSeq": 0, "score": 1},
                                  "M": {"active": True, "lastSeq": 4, "score": 2}})
    M = Member(rules, "M"); M.record_room(base)
    M.reboot()
    r = M.on_rs(base.copy())
    t.append(f"reboot; current v20 (== appliedV) delivered -> {r}; snapshot {'restored' if M.applied else 'ABSENT'}; ui='{M.ui}'")
    if M.applied is None or M.ui == "Reconnecting...":
        ok = False; t.append("  VIOLATION: recovery never completed although the current state was received")
    return ok, t

def scenario_running_equal_version_conflict(rules):
    """10: while running (snapshot in RAM), an RS with the SAME version but DIFFERENT content arrives."""
    t, ok = [], True
    base = RS("C", 7, 0xC3, 20, {"C": {"active": True, "lastSeq": 0, "score": 1},
                                  "M": {"active": True, "lastSeq": 4, "score": 2}})
    other = base.copy(); other.entries["M"]["score"] = 9
    M = Member(rules, "M"); M.record_room(base)
    r1 = M.on_rs(base.copy())
    r2 = M.on_rs(other)
    t.append(f"identical v20 again -> {r1}; different-content v20 -> {r2}; shown score {M.applied.entries['M']['score']}; ui='{M.ui}'")
    if r1 not in ("duplicate",) or r2 != "conflict":
        ok = False; t.append("  VIOLATION: equal-version handling while running is not 'duplicate if identical, conflict otherwise'")
    return ok, t

def scenario_equal_version_recovery_no_write(rules):
    """11: reboot; NVS writes would fail; the current RS is exactly appliedV. Recovery must succeed
    without attempting any NVS write."""
    t, ok = [], True
    base = RS("C", 7, 0xC3, 20, {"C": {"active": True, "lastSeq": 0, "score": 1},
                                  "M": {"active": True, "lastSeq": 4, "score": 2}})
    M = Member(rules, "M"); M.record_room(base)
    M.reboot()
    M.nvs_ok = False
    before = M.nvs_writes_attempted
    r = M.on_rs(base.copy())
    writes = M.nvs_writes_attempted - before
    t.append(f"reboot; NVS failing; current v20 == appliedV -> {r}; snapshot {'restored' if M.applied else 'ABSENT'}; "
             f"NVS writes attempted {writes}; ui='{M.ui}'")
    if M.applied is None or writes != 0:
        ok = False; t.append("  VIOLATION: equal-version recovery attempted a write and/or failed to restore")
    return ok, t

def scenario_conflict_freeze_enforced(rules):
    """12: after a same-version conflict the member is Leave-only: later packets must not advance the
    confirmed state or complete gameplay operations; Leave still works."""
    t, ok = [], True
    v20 = RS("C", 7, 0xC3, 20, {"C": {"active": True, "lastSeq": 0, "score": 1}})
    R = Member(rules, "R")
    q = R.join(v20)                                   # JOINING, snapshot v20, pending JOIN
    bad = v20.copy(); bad.entries["X"] = {"active": True, "lastSeq": 1, "score": 0}
    r1 = R.on_rs(bad)
    v21 = v20.copy(); v21.v = 21; v21.entries["R"] = {"active": True, "lastSeq": q.seq, "score": 0}
    r2 = R.on_rs(v21)
    t.append(f"JOINING at v20; same-version different content -> {r1}; then v21 (would admit R) -> {r2}")
    t.append(f"  after: shown v{R.applied.v}, state={R.rec['state']}, pending={'JOIN' if R.rec['pending'] else None}, "
             f"ui='{R.ui}', may start={R.can_act('start')}, may leave={R.can_act('leave')}")
    if r1 != "conflict":
        ok = False; t.append("  VIOLATION: conflict not detected")
    if R.applied.v != 20 or R.rec["state"] != "JOINING" or R.can_act("start"):
        ok = False; t.append("  VIOLATION: the frozen member advanced its confirmed state or completed a gameplay operation")
    if not R.can_act("leave"):
        ok = False; t.append("  VIOLATION: Leave not available while frozen")
    R.leave()
    t.append(f"  Leave -> state={R.rec['state']}")
    return ok, t

SCENARIOS = [
    # name, function, expected pass per rule set
    ("1a identity reuse: old JOIN/CLAIM before collision is visible", scenario_identity_reuse,
     {"r6": False, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("1b old member, NEW room's snapshot (old->new direction)", scenario_same_room_lower_snapshot,
     {"r6": False, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("2  member reboot: stale retained v18 after applied v20", scenario_member_reboot_stale,
     {"r6": False, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("3  FULL, reply lost, later snapshot after a leave", scenario_full_reply_lost,
     {"r6": False, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("C  control: normal reboot", scenario_normal_reboot,
     {"r6": True, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("4a NEW-room member, delayed OLD snapshot (new->old direction)", scenario_reverse_old_snapshot,
     {"r6": True, "f1": False, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("4b NEW-room member, old snapshot with HIGHER n", scenario_reverse_higher_n,
     {"r6": False, "f1": False, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("4c genuine replacement (closed 40, new 41) is recognised", scenario_genuine_supersession,
     {"r6": True, "f1": True, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("4d mismatched snapshot while the coordinator can't answer", scenario_verify_coordinator_offline,
     {"r6": True, "f1": False, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("5  appliedV write fails, reboot, stale v18", scenario_persist_failure,
     {"r6": False, "f1": False, "f2": True, "f3": True, "r7txt": True, "m4": True}),
    ("6  delayed QUERY reply after Leave A and Join B (same coordinator)", scenario_stale_query_reply_after_rejoin,
     {"r6": True, "f1": True, "f2": False, "f3": True, "r7txt": True, "m4": True}),
    ("7  lost QUERY: retry by replacement; late/foreign-group replies ignored", scenario_lost_query_recovery,
     {"r6": True, "f1": False, "f2": False, "f3": True, "r7txt": True, "m4": True}),
    ("8  reboot: QUERY invalidated, durable pending CLAIM resent identically", scenario_pending_survives_reboot,
     {"r6": True, "f1": True, "f2": True, "f3": True, "r7txt": False, "m4": True}),
    ("9  reboot then current state at exactly appliedV", scenario_reboot_exact_version,
     {"r6": True, "f1": True, "f2": True, "f3": True, "r7txt": False, "m4": True}),
    ("10 running: equal version identical -> duplicate, different -> conflict", scenario_running_equal_version_conflict,
     {"r6": False, "f1": False, "f2": False, "f3": True, "r7txt": True, "m4": True}),
    ("11 reboot recovery at exactly appliedV with NVS failing: no write, restored", scenario_equal_version_recovery_no_write,
     {"r6": True, "f1": False, "f2": False, "f3": True, "r7txt": False, "m4": False}),
    ("12 conflict freeze enforced: no later apply/completion; Leave works", scenario_conflict_freeze_enforced,
     {"r6": False, "f1": False, "f2": False, "f3": True, "r7txt": True, "m4": False}),
]

def main():
    print(RULES)
    all_good = True
    for name, fn, expect in SCENARIOS:
        for rules in ("r6", "f1", "f2", "f3", "r7txt", "m4"):
            ok, trace = fn(rules)
            match = (ok == expect[rules])
            all_good &= match
            print(f"[{rules}] {'PASS' if ok else 'FAIL'}  {name}   ({'as expected' if match else 'UNEXPECTED'})")
            for line in trace:
                print("      " + line)
        print()
    print("RESULT:", "every outcome matches the expectation table (all scenarios pass under f3)"
          if all_good else "MISMATCH")
    return 0 if all_good else 1

if __name__ == "__main__":
    sys.exit(main())
