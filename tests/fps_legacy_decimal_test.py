import pathlib, sqlite3, subprocess, tempfile

exe = pathlib.Path(__import__('sys').argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    local = root/'Client/Saved/LocalStorage'
    local.mkdir(parents=True)
    cfg = root/'Client/Saved/Config/WindowsNoEditor'
    cfg.mkdir(parents=True)
    ini = cfg/'GameUserSettings.ini'
    db = local/'LocalStorage.db'
    ini.write_bytes(b'[/Script/Engine.GameUserSettings]\r\nFrameRateLimit=60.000000\r\nOther=Keep\r\n')
    with sqlite3.connect(db) as d:
        d.execute('CREATE TABLE LocalStorage (key TEXT PRIMARY KEY, value TEXT)')
        d.executemany('INSERT INTO LocalStorage VALUES (?,?)', [('CustomFrameRate','2'),('MenuData','{}'),('PlayMenuInfo','{}')])
    def run(action):
        r=subprocess.run([exe,action,str(root)],capture_output=True,text=True)
        print(r.stdout.strip())
        assert r.returncode == 0
        return r.stdout
    run('on')
    ini.write_bytes(ini.read_bytes().replace(b'FrameRateLimit=120\r\n',b'FrameRateLimit=120.000000 ; user comment\r\n').replace(b'Other=Keep',b'Other=Changed'))
    run('off')
    assert b'FrameRateLimit=60.000000 ; user comment\r\n' in ini.read_bytes()
    assert b'Other=Changed\r\n' in ini.read_bytes()
    with sqlite3.connect(db) as d:
        d.execute("UPDATE LocalStorage SET value='120' WHERE key='CustomFrameRate'")
        d.execute('CREATE TABLE echo_fps_original_v1 (id INTEGER PRIMARY KEY CHECK(id=1), value)')
        d.execute("INSERT INTO echo_fps_original_v1 VALUES(1,'2')")
        d.execute("CREATE TRIGGER echo_fps_keep_120_v1 AFTER UPDATE OF value ON LocalStorage WHEN NEW.key = 'CustomFrameRate' AND CAST(NEW.value AS TEXT) IS NOT '120' BEGIN UPDATE LocalStorage SET value = 120 WHERE key = 'CustomFrameRate'; END")
    ini.write_bytes(ini.read_bytes().replace(b'FrameRateLimit=60.000000',b'FrameRateLimit=120.000000'))
    assert 'неизвестно' in run('on')
    with sqlite3.connect(db) as d:
        assert not d.execute("SELECT 1 FROM sqlite_master WHERE name='echo_fps_ini_v1'").fetchone()
    assert 'неизвестен' in run('off')
    assert b'FrameRateLimit=120.000000' in ini.read_bytes()
    print('PASS: numeric INI equivalence, unrelated changes and honest legacy restoration')
