cd /home/user/morse-buddy/host_tests/race_r3_r5; python3 - <<'EOF'
p='mutate_notify.py'; s=open(p).read()
old=''' (SM, "a feed overwrites a current cached index", "  if (g_indexCacheValid && g_indexCacheGeneration == now) return false;\\n", ""),'''
new=''' (SM, "a feed overwrites a current cached index", "  if ((g_indexCacheValid && g_indexCacheGeneration == now) || (g_loader.active", "  if ((g_loader.active"),'''
assert s.count(old)==1; s=s.replace(old,new); open(p,'w').write(s)
EOF
grep -n "g_loader.active && !g_loader.fed && g_loader.generation == now" mutate_notify.py ../../src/core/storage_messages.cpp