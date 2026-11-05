cd /home/user/morse-buddy/host_tests/race_r3_r5; python3 - <<'EOF'
p='mutate_notify.py'; s=open(p).read()
anchor=''' (SM, "a mutation elsewhere drops the parked index", "  if (keepParked) g_parked->generation = g_storageChangeGeneration;", ""),
'''
assert s.count(anchor)==1
add=anchor+''' (SM, "no parked feed while a chat holds the shared index", "    return beginParkedFeed(group_code, contact_key, seqs, n, listingReliable);", "    return false;"),
 (SM, "a mutation elsewhere drops the parked feed", "  if (keepParkedFeed) g_parked->feedGeneration = g_storageChangeGeneration;", ""),
 (SM, "a mutation of the fed conversation keeps the parked feed", "  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before &&\\n                        !parkedHolds(group_code, contact_key);", "  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before;"),
 (SM, "an unreadable record still completes the parked feed", "    p.valid = p.feedReliable;", "    p.valid = true;"),
 (SM, "an out-of-step record is fed to the parked index", "  if (p.feedPos >= p.feedN || p.feedSeqs[p.feedPos] != seq || p.feedGeneration != getStorageChangeGeneration()) {", "  if (p.feedPos >= p.feedN) {"),
 (SM, "the parked feed overwrites a current parked index", "  if (g_parked != nullptr && g_parked->valid && g_parked->generation == now) return false;  // current: kept", ""),
 (SM, "records are not fed to the parked index", "    feedParkedRecord(seq, hdr, message_id);\\n    return;", "    return;"),
'''
s=s.replace(anchor,add); open(p,'w').write(s)
EOF
grep -n "SM =\|^SM\b" mutate_notify.py | head -3