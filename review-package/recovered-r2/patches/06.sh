cd /root/.claude/projects/-home-user-morse-buddy/; python3 - <<'EOF'
import json,os,re
f='6c983299-def3-5558-ac59-7534d8753181.jsonl'
out='/home/user/morse-buddy/review-package/recovered-r2'; os.makedirs(out+'/patches',exist_ok=True)
lines=[json.loads(l) for l in open(f)]
def strip(t): return '\n'.join(re.sub(r'^\s*\d+\t','',x,count=1) for x in t.split('\n'))
for d in lines:
    if d.get('type')=='attachment' and d['attachment'].get('type')=='file':
        a=d['attachment']; s=json.dumps(a)
        c=a.get('content')
        txt=None
        if isinstance(c,dict):
            fi=c.get('file') or {}
            txt=fi.get('content') or c.get('content')
        if txt is None: print('keys',list(a.keys()), str(c)[:200]); continue
        fn=os.path.basename(a['filename']); open(out+'/'+fn,'w').write(txt); print('wrote',fn,len(txt))
k=0
for d in lines:
    m=d.get('message')
    if d.get('type')!='assistant' or not isinstance(m,dict): continue
    for c in m.get('content',[]):
        if c.get('type')!='tool_use': continue
        inp=c['input']
        if c['name']=='Write':
            fn=os.path.basename(inp['file_path']); open(out+'/'+fn,'w').write(inp['content']); print('wrote',fn)
        elif c['name']=='Bash' and "python3 - <<'EOF'" in inp['command'] and ('rep(' in inp['command'] or 's.replace' in inp['command']) and ('storage_messages' in inp['command'] or 'storage_test' in inp['command'] or 'mutate_notify' in inp['command']):
            k+=1; open(f'{out}/patches/{k:02d}.sh','w').write(inp['command']); print('patch',k)
EOF
ls -la /home/user/morse-buddy/review-package/recovered-r2 /home/user/morse-buddy/review-package/recovered-r2/patches