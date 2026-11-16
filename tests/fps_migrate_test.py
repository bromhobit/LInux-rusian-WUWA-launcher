import tempfile,pathlib,sqlite3,subprocess
exe=pathlib.Path(__import__('sys').argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
 r=pathlib.Path(tmp); l=r/'Client/Saved/LocalStorage';l.mkdir(parents=True); c=r/'Client/Saved/Config/WindowsNoEditor';c.mkdir(parents=True); ini=c/'GameUserSettings.ini';ini.write_text('[/Script/Engine.GameUserSettings]\nFrameRateLimit=60\nOther=1\n'); db=l/'LocalStorage.db'
 with sqlite3.connect(db) as d:
  d.execute('CREATE TABLE LocalStorage (key TEXT PRIMARY KEY, value TEXT)');d.execute("INSERT INTO LocalStorage VALUES('CustomFrameRate','120')"); d.execute('CREATE TABLE echo_fps_original_v1 (id INTEGER PRIMARY KEY CHECK(id=1), value)'); d.execute("INSERT INTO echo_fps_original_v1 VALUES(1,'60')");d.execute("CREATE TRIGGER echo_fps_keep_120_v1 AFTER UPDATE OF value ON LocalStorage WHEN NEW.key = 'CustomFrameRate' AND CAST(NEW.value AS TEXT) IS NOT '120' BEGIN UPDATE LocalStorage SET value = 120 WHERE key = 'CustomFrameRate'; END")
 with sqlite3.connect(db) as d: d.executemany('INSERT INTO LocalStorage VALUES (?,?)', [('MenuData','{}'),('PlayMenuInfo','{}')])
 def run(mode):
  p=subprocess.run([exe,mode,str(r)],text=True,capture_output=True);print(p.stdout.strip());assert p.returncode==0
 run('on');assert 'FrameRateLimit=120' in ini.read_text()
 # Simulate interruption after database commit and before INI write.
 ini.write_text(ini.read_text().replace('FrameRateLimit=120','FrameRateLimit=60'))
 run('on');assert 'FrameRateLimit=120' in ini.read_text()
 # Simulate interruption after INI restore and before database commit.
 ini.write_text(ini.read_text().replace('FrameRateLimit=120','FrameRateLimit=60'))
 run('off');assert 'FrameRateLimit=60' in ini.read_text()
 with sqlite3.connect(db) as d: assert d.execute("SELECT value FROM LocalStorage WHERE key='CustomFrameRate'").fetchone()[0]=='60'
 print('PASS: migration and interrupted operations')
