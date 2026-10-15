# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real fault injection limited to owned fixture clients; no host routing changes."""
import json
import os
from pathlib import Path
import subprocess
import signal
import time
import sys
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools"))
from test_environment.config import Config, STATE, EnvironmentError
from test_environment.fixture import private_write, probe_database, runtime_command, prepare_kafka, prepare_databases, wait_databases
from test_environment.runner import existing_kafka_manifest,kafka_probe

config=Config()
root=STATE/"acceptance"/uuid.uuid4().hex
root.mkdir(parents=True,mode=0o700)
passed=[]

def modified(change):
    data=json.loads(json.dumps(config.data))
    for item in [*data['databases'].values(),data['kafka']]:
        if item['password_ref'].startswith('file:'):
            item['password_ref']='file:'+str(config.secret_path(item['password_ref']))
    data['managed']=False
    change(data)
    path=root/(uuid.uuid4().hex+'.json');private_write(path,json.dumps(data));return Config(path)

def failure(label,expected,call):
    try:call()
    except EnvironmentError as error:
        assert error.category==expected,(label,error.result())
        passed.append(label);print('PASS',label,error.category,flush=True)
    else:raise AssertionError(label+' unexpectedly succeeded')

wrong=root/'wrong-password';private_write(wrong,'negative-fixture-secret')
c=modified(lambda data:data['databases']['postgres'].update(password_ref='file:'+str(wrong)))
failure('wrong-password','authentication_failed',lambda:probe_database(c,'postgres','driver','docker'))
c=modified(lambda data:data['databases']['postgres']['roles'].update(driver='test_environment_missing'))
failure('missing-database','database_missing',lambda:probe_database(c,'postgres','driver','docker'))
c=modified(lambda data:data['databases']['mysql'].update(password_ref='file:'+str(root/'missing')))
failure('missing-secret-reference','config_missing',lambda:c.secret(c.data['databases']['mysql']['password_ref']))
bad=root/'bad.json';private_write(bad,'{"schema_version":1,"schema_version":1}')
failure('duplicate-config','config_invalid',lambda:Config(bad))
# Published host port becomes unavailable only in a config copy; Docker service mapping stays unchanged.
c=modified(lambda data:data['databases']['postgres']['endpoints']['native'].update(port=1))
failure('native-unreachable','network_unreachable',lambda:probe_database(c,'postgres','driver','native'))
assert probe_database(c,'postgres','driver','docker')['category']=='ready'
passed.append('docker-independent-of-published-port');print('PASS docker-independent-of-published-port',flush=True)

# Create a readonly test principal inside only the owned PostgreSQL fixture, then prove write denial.
assert config.data['managed'],'fault suite only uses owned resources'
password=config.secret(config.data['databases']['postgres']['password_ref'])
username='test_readonly_'+uuid.uuid4().hex[:8]
database=config.data['databases']['postgres']['roles']['driver']
container=config.data['project'].replace('_','-')+'-postgres-1'
user=config.data['databases']['postgres']['user']
sql=f"CREATE ROLE {username} LOGIN PASSWORD '{password}'; GRANT CONNECT ON DATABASE {database} TO {username};"
r=subprocess.run(['docker','exec','-i',container,'psql','-v','ON_ERROR_STOP=1','-U',user,'-d',database],input=sql,capture_output=True,text=True)
assert r.returncode==0,'owned permission fixture preparation failed'
try:
    c=modified(lambda data:data['databases']['postgres'].update(user=username))
    assert probe_database(c,'postgres','driver','docker')['category']=='ready'
    failure('permission-denied','permission_denied',lambda:probe_database(c,'postgres','driver','docker',True,10))
finally:
    subprocess.run(['docker','exec','-i',container,'psql','-U',user,'-d',database],
                   input=f'DROP OWNED BY {username}; DROP ROLE {username};',capture_output=True,text=True,check=True)

location='docker'
manifest=existing_kafka_manifest(config,location)
profiles=json.loads(manifest.read_text())
def kafka_failure(label,expected,profile,change):
    data=json.loads(json.dumps(profiles))
    change(data[profile])
    path=root/(label+'.json');private_write(path,json.dumps(data))
    failure(label,expected,lambda:kafka_probe(config,location,profile,path))
kafka_failure('kafka-password','authentication_failed','PLAIN',lambda p:p['connection']['credential_bindings']['password'].update(value='negative-fixture-secret'))
kafka_failure('kafka-topic-missing','topic_missing','PLAINTEXT',lambda p:p.update(topic='test_environment_missing_topic'))
kafka_failure('kafka-wrong-ca','tls_validation_failed','SSL',lambda p:p['files'].update(ca=profiles['MTLS']['files']['certificate']))
kafka_failure('kafka-token-service','token_service_unavailable','OAUTHBEARER',lambda p:p['connection']['sasl'].update(token_endpoint='https://kafka:1/token'))
# A served manifest must advertise addresses reachable from the actual client. This failed metadata-only probe cannot create Topic/group.
kafka_failure('kafka-unreachable-broker','network_unreachable','PLAINTEXT',lambda p:p['connection'].update(bootstrap_servers=['unreachable-broker.invalid:1']))
# A completely new project and secret tree works without exports or usable published ports.
suffix=uuid.uuid4().hex[:8]
def fresh(data):
    data.update(project='flowsql_empty_'+suffix,managed=True)
    for backend,item in data['databases'].items():
        item['password_ref']='file:@state/empty-'+suffix+'/secrets/'+backend
        item['endpoints']['native']['port']=1
    data['kafka']['password_ref']='file:@state/empty-'+suffix+'/secrets/kafka'
c=modified(fresh)
deadline=time.monotonic()+60
compose=STATE/c.data['project']/'compose.json'
try:
    prepare_databases(c,['mysql','postgres','clickhouse'],deadline,'docker')
    wait_databases(c,['mysql','postgres','clickhouse'],'docker',deadline)
    document=json.loads(compose.read_text())
    assert all('ports' not in service for service in document['services'].values())
    for backend in ['mysql','postgres','clickhouse']:
        assert probe_database(c,backend,'driver','docker',True,10)['permissions']=='verified'
    passed.append('empty-state-no-export-no-published-port')
    print('PASS empty-state-no-export-no-published-port',flush=True)
finally:
    if compose.exists():subprocess.run(['docker','compose','-f',str(compose),'stop'],capture_output=True,timeout=60)

# Valid bootstrap and TLS, but the returned advertised SSL broker is unreachable.
c=modified(lambda data:data.update(project='flowsql_advertised_'+uuid.uuid4().hex[:8],managed=True))
manifest=prepare_kafka(c,'native',invalid_advertised=True)
identity=STATE/c.data['project']/'kafka-native-identity.json'
try:
    failure('kafka-advertised-address','broker_address_unreachable',lambda:kafka_probe(c,'native','SSL',manifest))
finally:
    pid=json.loads(identity.read_text())['pid']
    os.kill(pid,signal.SIGTERM)
    deadline=time.monotonic()+25
    while Path('/proc/'+str(pid)).exists() and time.monotonic()<deadline:time.sleep(.1)
print(json.dumps({'cases':len(passed),'passed':passed,'skipped':0}),flush=True)
