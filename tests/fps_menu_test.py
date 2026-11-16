import tempfile,pathlib,sqlite3,subprocess
exe=pathlib.Path(__import__('sys').argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp); child=root/'Wuthering Waves Game'; child.mkdir()
 def db(root, values):
  l=root/'Client/Saved/LocalStorage';l.mkdir(parents=True); p=l/'LocalStorage.db'
  with sqlite3.connect(p) as d:
   d.execute('CREATE TABLE LocalStorage (key TEXT PRIMARY KEY, value TEXT)');d.executemany('INSERT INTO LocalStorage VALUES (?,?)',values)
  return p
 src=db(root,[('MenuData','{"foo":1}'),('PlayMenuInfo','{"bar":2}')]); dest=db(child,[('CustomFrameRate','60')]); c=child/'Client/Saved/Config/WindowsNoEditor';c.mkdir(parents=True); ini=c/'GameUserSettings.ini';ini.write_text('[/Script/Engine.GameUserSettings]\nFrameRateLimit=60\n')
 def run(mode,expected=True):
  p=subprocess.run([exe,mode,str(child)],text=True,capture_output=True);print(p.stdout.strip());assert (p.returncode==0)==expected
 run('on'); run('on')
 with sqlite3.connect(dest) as d:
  assert dict(d.execute('select key,value from LocalStorage'))=={'CustomFrameRate':'120','MenuData':'{"foo":1}','PlayMenuInfo':'{"bar":2}'}
  d.execute("UPDATE LocalStorage SET value=? WHERE key='MenuData'",('{"updated":3}',))
 run('off')
 with sqlite3.connect(dest) as d:
  assert dict(d.execute('select key,value from LocalStorage'))=={'CustomFrameRate':'60','MenuData':'{"updated":3}'}
  assert d.execute("select count(*) from sqlite_master where name='echo_fps_menu_v1'").fetchone()[0]==0
 run('on');run('off')
 with sqlite3.connect(dest) as d: assert dict(d.execute('select key,value from LocalStorage'))=={'CustomFrameRate':'60','MenuData':'{"updated":3}'}
 with sqlite3.connect(src) as d: d.execute("delete from LocalStorage where key='PlayMenuInfo'")
 run('on',False)
 with sqlite3.connect(dest) as d: assert dict(d.execute('select key,value from LocalStorage'))=={'CustomFrameRate':'60','MenuData':'{"updated":3}'}
 print('PASS: missing keys, no overwrite, restore unchanged only, failed source rollback')
