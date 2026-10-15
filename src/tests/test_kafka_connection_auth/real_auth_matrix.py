# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import shutil, argparse, sys, uuid, atexit, signal
import os, pathlib, subprocess, socket, time, json, ssl, http.server, threading, urllib.parse, base64
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import hashes, serialization
repo=pathlib.Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description="Real Kafka 4.1.0 authentication matrix; requires JRE21, MIT KDC, cryptography, openssl and Docker for container cases")
parser.add_argument("--kafka-home",default=os.environ.get("FLOWSQL_KAFKA_HOME"),required=not os.environ.get("FLOWSQL_KAFKA_HOME"))
parser.add_argument("--mode",choices=["native","docker"],default="native")
parser.add_argument("--config")
parser.add_argument("--network")
parser.add_argument("--host")
parser.add_argument("--invalid-advertised",action="store_true",help="Owned negative metadata fixture")
parser.add_argument("--output")
parser.add_argument("--serve",action="store_true",help="Prepare isolated services and keep them available for read-only probes")
parser.add_argument("--client-image",help="Also run clients and real HTTP E2E in this FlowSQL runtime image")
args=parser.parse_args()
sys.path.insert(0,str(repo/"tools"))
from test_environment.config import Config, STATE
configuration=Config(args.config)
settings=configuration.data["kafka"]
location="docker" if args.network else "native"
host=args.host or settings["hosts"][location]
secret=configuration.secret(settings["password_ref"])
os.umask(0o077)
root=(pathlib.Path(args.output) if args.output else STATE/configuration.data["project"]/("kafka-"+uuid.uuid4().hex)).resolve()
children=[]
def cleanup():
 if args.client_image:
  active=subprocess.run(["docker","ps","-q","--filter","label=flowsql.test.run="+root.name],capture_output=True,text=True)
  for container in active.stdout.split():subprocess.run(["docker","stop","-t","5",container],capture_output=True)
 for child in reversed(children):
  if child.poll() is None:
   child.terminate()
   try: child.wait(timeout=20)
   except subprocess.TimeoutExpired: child.kill();child.wait()
atexit.register(cleanup)
def interrupted(*_): raise SystemExit(130)
signal.signal(signal.SIGTERM,interrupted)
signal.signal(signal.SIGINT,interrupted)
root.mkdir(parents=True,exist_ok=True)
# Java tools need a stable private Linux working directory throughout the fixture.
os.chdir(root)
kafka=pathlib.Path(args.kafka_home).resolve()
assert (kafka/"libs/kafka-clients-4.1.0.jar").exists(),"Kafka fixture must be version 4.1.0"
shutil.copy2(repo/"build/output/test_kafka_connection_auth",root/"test-client")
java="/usr/lib/jvm/java-21-openjdk-amd64"
env=dict(os.environ, JAVA_HOME=java, KAFKA_HEAP_OPTS="-Xmx512m -Xms128m", KAFKA_GC_LOG_OPTS="-Xlog:gc:file="+str(root/"gc.log"), FLOWSQL_FIXTURE_PASSWORD=secret)
ports={}
for name in ["plain","ssl","mtls","sasl","saslssl","oauth","gss","controller","idp"]:
 while True:
  s=socket.socket();s.bind(("127.0.0.1",0));candidate=s.getsockname()[1];s.close()
  if candidate not in ports.values():ports[name]=candidate;break
def run(args,**kwargs):
 result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=60,**kwargs)
 if result.returncode:
  (root/"last-command.log").write_text((result.stdout+result.stderr).replace(secret,"[REDACTED]"))
  raise RuntimeError(str(args[:2])+" failed (exit "+str(result.returncode)+"): "+"[private fixture failure]")
 return result.stdout
cert=root/"tls";cert.mkdir(exist_ok=True)
if not (cert/"server.p12").exists():
 run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-keyout",str(cert/"ca.key"),"-out",str(cert/"ca.pem"),"-days","2","-subj","/CN=FlowSQL Test CA"])
 for name in ["server","client"]:
  run(["openssl","req","-newkey","rsa:2048","-nodes","-keyout",str(cert/(name+".key")),"-out",str(cert/(name+".csr")),"-subj","/CN="+host])
  (cert/(name+".ext")).write_text("subjectAltName=DNS:"+host+"\nextendedKeyUsage=serverAuth,clientAuth\n")
  run(["openssl","x509","-req","-in",str(cert/(name+".csr")),"-CA",str(cert/"ca.pem"),"-CAkey",str(cert/"ca.key"),"-CAcreateserial","-out",str(cert/(name+".pem")),"-days","2","-extfile",str(cert/(name+".ext"))])
 run(["openssl","pkcs12","-export","-in",str(cert/"server.pem"),"-inkey",str(cert/"server.key"),"-certfile",str(cert/"ca.pem"),"-out",str(cert/"server.p12"),"-passout","env:FLOWSQL_FIXTURE_PASSWORD"])
 run([java+"/bin/keytool","-importcert","-noprompt","-alias","ca","-file",str(cert/"ca.pem"),"-keystore",str(cert/"trust.p12"),"-storepass:env","FLOWSQL_FIXTURE_PASSWORD","-storetype","PKCS12"])
# Isolated KDC and signed HTTPS OAuth provider, never production services.
kerberos=root/"kerberos";kerberos.mkdir(exist_ok=True)
while True:
 sock=socket.socket();sock.bind(("127.0.0.1",0));kdc_port=sock.getsockname()[1];sock.close()
 if kdc_port not in ports.values():break
(kerberos/"krb5.conf").write_text(f"""[libdefaults]
 default_realm = FLOWSQL.TEST
 dns_lookup_kdc = false
 dns_lookup_realm = false
 rdns = false
[realms]
 FLOWSQL.TEST = {{
  kdc = {host}:{kdc_port}
 }}
""")
(kerberos/"kdc.conf").write_text(f"""[kdcdefaults]
 kdc_ports = {kdc_port}
 kdc_tcp_ports = {kdc_port}
[realms]
 FLOWSQL.TEST = {{
 database_name = {kerberos}/principal
 key_stash_file = {kerberos}/stash
 acl_file = {kerberos}/acl
 max_life = 2m
 }}
""")
env.update(KRB5_CONFIG=str(kerberos/"krb5.conf"),KRB5_KDC_PROFILE=str(kerberos/"kdc.conf"))
env["KAFKA_OPTS"]=f"-Djava.security.krb5.conf={kerberos}/krb5.conf -Dorg.apache.kafka.sasl.oauthbearer.allowed.urls=https://{host}:{ports['idp']}/jwks"
if not (kerberos/"principal").exists():
 subprocess.run(["kdb5_util","create","-s","-r","FLOWSQL.TEST"],env=env,input=secret+"\n"+secret+"\n",text=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
 for principal,file in [("alice","alice.keytab"),("bob","bob.keytab"),("kafka/"+host,"server.keytab")]:
  run(["kadmin.local","-r","FLOWSQL.TEST","-q","addprinc -randkey "+principal])
  run(["kadmin.local","-r","FLOWSQL.TEST","-q","ktadd -k "+str(kerberos/file)+" "+principal])
kdc_log=(kerberos/"kdc.log").open("w")
kdc=subprocess.Popen(["krb5kdc","-n","-r","FLOWSQL.TEST"],env=env,stdout=kdc_log,stderr=kdc_log)
children.append(kdc)
private=rsa.generate_private_key(public_exponent=65537,key_size=2048)
def b64(data):return base64.urlsafe_b64encode(data).rstrip(b"=").decode()
def number(n):return b64(n.to_bytes((n.bit_length()+7)//8,"big"))
public=private.public_key().public_numbers()
jwks={"keys":[{"kty":"RSA","kid":"flowsql-fixture","use":"sig","alg":"RS256","n":number(public.n),"e":number(public.e)}]}
token_requests=[]
idp_failing=False
class OAuthHandler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  data=json.dumps(jwks).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
 def do_POST(self):
  form=urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("content-length","0"))).decode())
  token_requests.append(time.time())
  if idp_failing or form.get("client_secret")!=[secret]:
   self.send_response(401);self.end_headers();return
  now=int(time.time());header=b64(json.dumps({"alg":"RS256","kid":"flowsql-fixture"}).encode())
  claims=b64(json.dumps({"sub":"tester","iat":now,"exp":now+20,"aud":"flowsql"}).encode());message=(header+"."+claims).encode()
  token=message.decode()+"."+b64(private.sign(message,padding.PKCS1v15(),hashes.SHA256()))
  data=json.dumps({"access_token":token,"token_type":"Bearer","expires_in":20}).encode()
  self.send_response(200);self.send_header("Content-Type","application/json");self.end_headers();self.wfile.write(data)
idp=http.server.ThreadingHTTPServer(("0.0.0.0",ports['idp']),OAuthHandler)
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert/"server.pem",cert/"server.key")
idp.socket=context.wrap_socket(idp.socket,server_side=True);threading.Thread(target=idp.serve_forever,daemon=True).start()
(root/"ports.json").write_text(json.dumps(ports))
config={"process.roles":"broker,controller","node.id":"1","controller.quorum.voters":f"1@{host}:{ports['controller']}","controller.listener.names":"CONTROLLER","listeners":",".join(f"{n.upper()}://{host}:{p}" for n,p in ports.items() if n not in ["idp"]),"advertised.listeners":",".join(f"{n.upper()}://{host}:{p}" for n,p in ports.items() if n not in ["idp","controller"]),"listener.security.protocol.map":"PLAIN:PLAINTEXT,SSL:SSL,MTLS:SSL,SASL:SASL_PLAINTEXT,SASLSSL:SASL_SSL,OAUTH:SASL_SSL,GSS:SASL_PLAINTEXT,CONTROLLER:PLAINTEXT","inter.broker.listener.name":"PLAIN","log.dirs":str(root/"data"),"offsets.topic.replication.factor":"1","transaction.state.log.replication.factor":"1","transaction.state.log.min.isr":"1","group.initial.rebalance.delay.ms":"0","auto.create.topics.enable":"true","ssl.keystore.type":"PKCS12","ssl.keystore.location":str(cert/"server.p12"),"ssl.keystore.password":secret,"ssl.key.password":secret,"ssl.truststore.type":"PKCS12","ssl.truststore.location":str(cert/"trust.p12"),"ssl.truststore.password":secret,"listener.name.mtls.ssl.client.auth":"required","sasl.enabled.mechanisms":"PLAIN,SCRAM-SHA-256,SCRAM-SHA-512,GSSAPI,OAUTHBEARER"}
if args.invalid_advertised:
 config["advertised.listeners"]=config["advertised.listeners"].replace(f"SSL://{host}:{ports['ssl']}",f"SSL://unreachable-broker.invalid:{ports['ssl']}")
for listener in ["sasl","saslssl"]:
 config[f"listener.name.{listener}.sasl.enabled.mechanisms"]="PLAIN,SCRAM-SHA-256,SCRAM-SHA-512"
 config[f"listener.name.{listener}.plain.sasl.jaas.config"]=f'org.apache.kafka.common.security.plain.PlainLoginModule required username="tester" password="{secret}" user_tester="{secret}";'
 for mech in ["scram-sha-256","scram-sha-512"]:
  config[f"listener.name.{listener}.{mech}.sasl.jaas.config"]=f'org.apache.kafka.common.security.scram.ScramLoginModule required username="tester" password="{secret}";'
config.update({"listener.name.gss.sasl.enabled.mechanisms":"GSSAPI","listener.name.gss.gssapi.sasl.jaas.config":f'com.sun.security.auth.module.Krb5LoginModule required useKeyTab=true storeKey=true doNotPrompt=true keyTab="{kerberos}/server.keytab" principal="kafka/{host}@FLOWSQL.TEST";',"sasl.kerberos.service.name":"kafka","listener.name.oauth.sasl.enabled.mechanisms":"OAUTHBEARER","listener.name.oauth.oauthbearer.sasl.jaas.config":"org.apache.kafka.common.security.oauthbearer.OAuthBearerLoginModule required;","listener.name.oauth.oauthbearer.sasl.server.callback.handler.class":"org.apache.kafka.common.security.oauthbearer.OAuthBearerValidatorCallbackHandler","listener.name.oauth.sasl.oauthbearer.jwks.endpoint.url":f"https://{host}:{ports['idp']}/jwks","listener.name.oauth.sasl.oauthbearer.expected.audience":"flowsql","connections.max.reauth.ms":"15000","authorizer.class.name":"org.apache.kafka.metadata.authorizer.StandardAuthorizer","allow.everyone.if.no.acl.found":"true","super.users":"User:ANONYMOUS"})
(root/"server.properties").write_text("".join(f"{k}={v}\n" for k,v in config.items()))
if not (root/"data/meta.properties").exists():
 cluster=run([str(kafka/"bin/kafka-storage.sh"),"random-uuid"]).strip()
 run([str(kafka/"bin/kafka-storage.sh"),"format","-t",cluster,"-c",str(root/"server.properties")])
log=(root/"broker.log").open("w")
broker_command=[str(kafka/"bin/kafka-server-start.sh"),str(root/"server.properties")]
process=subprocess.Popen(broker_command,env=env,stdout=log,stderr=log)
children.append(process)
def client_command(environment,web=False):
 binary=repo/"build/output"/("test_kafka_web_e2e" if web else "test_kafka_connection_auth")
 if not args.client_image:return [str(binary) if web else str(root/"test-client")]
 command=["docker","run","--rm","--network",args.network or "host","--label","flowsql.test.run="+root.name,"-v",str(repo)+":"+str(repo)+":ro","-v",str(root)+":"+str(root),"-v",str(repo/"build/output")+":/opt/flowsql/bin:ro"]
 for key,value in environment.items():
  if key.startswith("FLOWSQL_KAFKA_") or key in ["KRB5_CONFIG","KRB5CCNAME"]:command += ["-e",key+"="+value]
 return command+[args.client_image,"/opt/flowsql/bin/"+binary.name]

try:
 for _ in range(200):
  if process.poll() is not None: raise RuntimeError("broker failed; see broker.log")
  try:
   s=socket.create_connection((host,ports["plain"]),timeout=.2);s.close();break
  except OSError:time.sleep(.2)
 for mechanism in ["SCRAM-SHA-256","SCRAM-SHA-512"]:
  (root/"scram.properties").write_text(f"{mechanism}=password={secret}\n")
  run([str(kafka/"bin/kafka-configs.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--alter","--entity-type","users","--entity-name","tester","--add-config-file",str(root/"scram.properties")])
 run([str(kafka/"bin/kafka-topics.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--create","--if-not-exists","--topic",settings["topic"],"--partitions","1","--replication-factor","1"])
 for topic in [settings["second_topic"]]:
  run([str(kafka/"bin/kafka-topics.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--create","--if-not-exists","--topic",topic,"--partitions","1","--replication-factor","1"])
 for topic in [settings["topic"],settings["second_topic"]]:
  run([str(kafka/"bin/kafka-acls.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--add","--allow-principal","User:*","--operation","Describe","--topic",topic])
 for principal,topic in [("bob",settings["topic"]),("alice",settings["second_topic"])]:
  run([str(kafka/"bin/kafka-acls.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--add","--deny-principal","User:"+principal,"--operation","Describe","--topic",topic])
 results=[]
 cases=[("PLAINTEXT","plain",None),("SSL","ssl",None),("MTLS","mtls",None)]+[(protocol,listener,mechanism) for protocol,listener in [("SASL_PLAINTEXT","sasl"),("SASL_SSL","saslssl")] for mechanism in ["PLAIN","SCRAM-SHA-256","SCRAM-SHA-512"]]
 cases += [("SASL_PLAINTEXT","gss","GSSAPI"),("SASL_SSL","oauth","OAUTHBEARER")]
 def definition(mode,listener,mechanism):
  connection={"name":"real","bootstrap_servers":[f"{host}:{ports[listener]}"],"security_protocol":"SSL" if mode=="MTLS" else mode}
  if mode in ["SSL","MTLS","SASL_SSL"]:connection["tls"]={"ca_ref":"file:ca"}
  if mode=="MTLS":connection["tls"].update(certificate_ref="file:certificate",private_key_ref="file:key")
  if mechanism:connection.update(sasl_mechanism=mechanism,sasl={"username":"tester"},credential_bindings={"password":{"operation":"replace","value":secret}})
  if mechanism=="GSSAPI":connection.update(sasl={"principal":"alice@FLOWSQL.TEST","service_name":"kafka","keytab_ref":"file:keytab"},credential_bindings={})
  if mechanism=="OAUTHBEARER":connection.update(sasl={"client_id":"tester","token_endpoint":f"https://{host}:{ports['idp']}/token","ca_ref":"file:oauth_ca"},credential_bindings={"client_secret":{"operation":"replace","value":secret}})
  return connection
 if args.serve:
  profiles={}
  for mode,listener,mechanism in cases:
   label=mechanism or mode
   if label in profiles:label=mode+"-"+mechanism
   profiles[label]={"connection":definition(mode,listener,mechanism),"files":{"ca":str(cert/"ca.pem"),"certificate":str(cert/"client.pem"),"key":str(cert/"client.key"),"keytab":str(kerberos/"alice.keytab"),"oauth_ca":str(cert/"ca.pem")},"topic":settings["topic"],"runtime":str(root/"probe-runtime")}
  (root/"profiles.json").write_text(json.dumps(profiles))
  print("READY isolated Kafka profiles",flush=True)
  while True:time.sleep(1)
 for mode,listener,mechanism in cases:
  connection=definition(mode,listener,mechanism)
  path=root/"client.json";path.write_text(json.dumps(connection))
  callenv=dict(env,FLOWSQL_KAFKA_CONNECTION_JSON=str(path),FLOWSQL_KAFKA_TOPIC=settings["topic"],FLOWSQL_KAFKA_SECOND_TOPIC=settings["second_topic"],FLOWSQL_KAFKA_RUNTIME=str(root/"client-runtime"),FLOWSQL_KAFKA_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_CERT=str(cert/"client.pem"),FLOWSQL_KAFKA_KEY=str(cert/"client.key"))
  if mechanism=="GSSAPI":callenv.update(FLOWSQL_KAFKA_KEYTAB=str(kerberos/"alice.keytab"),FLOWSQL_KAFKA_SECOND_KEYTAB=str(kerberos/"bob.keytab"),FLOWSQL_KAFKA_REPEAT_SECONDS="70")
  if mechanism=="OAUTHBEARER":callenv.update(FLOWSQL_KAFKA_OAUTH_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_REPEAT_SECONDS="35")
  result=subprocess.run(client_command(callenv),env=callenv,capture_output=True,text=True,timeout=100)
  (root/(mode+"-"+str(mechanism)+".log")).write_text(result.stdout+result.stderr)
  if result.returncode:raise RuntimeError(mode+" "+str(mechanism)+" failed")
  web=subprocess.run(client_command(callenv,web=True),env=callenv,capture_output=True,text=True,timeout=45)
  (root/(mode+"-"+str(mechanism)+"-web.log")).write_text(web.stdout+web.stderr)
  assert web.returncode==0,mode+" "+str(mechanism)+" real HTTP E2E failed"
  results.append([mode,mechanism,"passed"]);print(mode,mechanism,"client and HTTP passed",flush=True)
 # Failure cases and absence of Topic-creation side effects.
 failures=[("wrong-password",{"name":"real","bootstrap_servers":[f"{host}:{ports['saslssl']}"],"security_protocol":"SASL_SSL","tls":{"ca_ref":"file:ca"},"sasl_mechanism":"PLAIN","sasl":{"username":"tester"},"credential_bindings":{"password":{"operation":"replace","value":"incorrect-secret"}}},{}),
 ("unsupported-mechanism",{"name":"real","bootstrap_servers":[f"{host}:{ports['gss']}"],"security_protocol":"SASL_PLAINTEXT","sasl_mechanism":"PLAIN","sasl":{"username":"tester"},"credential_bindings":{"password":{"operation":"replace","value":secret}}},{}),
 ("hostname",{"name":"real","bootstrap_servers":[f"127.0.0.1:{ports['ssl']}"],"security_protocol":"SSL","tls":{"ca_ref":"file:ca"}},{}),
 ("missing-client-certificate",{"name":"real","bootstrap_servers":[f"{host}:{ports['mtls']}"],"security_protocol":"SSL","tls":{"ca_ref":"file:ca"}},{}),
 ("wrong-ca",{"name":"real","bootstrap_servers":[f"{host}:{ports['ssl']}"],"security_protocol":"SSL","tls":{"ca_ref":"file:ca"}},{"FLOWSQL_KAFKA_CA":str(cert/"client.pem")}),
 ("missing-topic",{"name":"real","bootstrap_servers":[f"{host}:{ports['plain']}"],"security_protocol":"PLAINTEXT"},{"FLOWSQL_KAFKA_TOPIC":"probe-must-not-create"}),
 ("oauth-secret",connection,{})]
 for label,definition,extra in failures:
  if label=="oauth-secret":definition["credential_bindings"]["client_secret"]["value"]="incorrect-secret"
  path=root/"client.json";path.write_text(json.dumps(definition))
  failureenv=dict(env,FLOWSQL_KAFKA_CONNECTION_JSON=str(path),FLOWSQL_KAFKA_TOPIC=settings["topic"],FLOWSQL_KAFKA_SECOND_TOPIC=settings["second_topic"],FLOWSQL_KAFKA_RUNTIME=str(root/"client-runtime"),FLOWSQL_KAFKA_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_OAUTH_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_EXPECT_FAILURE="1");failureenv.update(extra)
  outcome=subprocess.run(client_command(failureenv),env=failureenv,capture_output=True,text=True,timeout=20)
  (root/(label+".log")).write_text(outcome.stdout+outcome.stderr)
  assert outcome.returncode==0,label+" must fail safely"
  assert "incorrect-secret" not in outcome.stdout+outcome.stderr
  results.append([label,"failure","passed"]);print(label,"passed",flush=True)
 topics=run([str(kafka/"bin/kafka-topics.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--list"])
 assert "probe-must-not-create" not in topics
 groups=run([str(kafka/"bin/kafka-consumer-groups.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--list"])
 assert not groups.strip(),"metadata probes must not join consumer groups"
 for topic in [settings["topic"],settings["second_topic"]]:
  offsets=run([str(kafka/"bin/kafka-get-offsets.sh"),"--bootstrap-server",f"{host}:{ports['plain']}","--topic",topic])
  assert offsets.strip()==topic+":0:0","metadata probes must not publish records"
 print("PASS metadata-only probes: no created Topic, consumer groups or published records",flush=True)
 # Renewal failures are checked after an initially successful connection.
 for mechanism in ["OAUTHBEARER","GSSAPI"]:
  definition={"name":"real","bootstrap_servers":[f"{host}:{ports['oauth' if mechanism=='OAUTHBEARER' else 'gss']}"],"security_protocol":"SASL_SSL" if mechanism=="OAUTHBEARER" else "SASL_PLAINTEXT","sasl_mechanism":mechanism}
  if mechanism=="OAUTHBEARER":
   definition.update(tls={"ca_ref":"file:ca"},sasl={"client_id":"tester","token_endpoint":f"https://{host}:{ports['idp']}/token","ca_ref":"file:oauth_ca"},credential_bindings={"client_secret":{"operation":"replace","value":secret}})
  else:definition.update(sasl={"principal":"alice@FLOWSQL.TEST","service_name":"kafka","keytab_ref":"file:keytab"})
  path=root/"renewal.json";path.write_text(json.dumps(definition))
  renewenv=dict(env,FLOWSQL_KAFKA_CONNECTION_JSON=str(path),FLOWSQL_KAFKA_TOPIC=settings["topic"],FLOWSQL_KAFKA_SECOND_TOPIC=settings["second_topic"],FLOWSQL_KAFKA_RUNTIME=str(root/"client-runtime"),FLOWSQL_KAFKA_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_OAUTH_CA=str(cert/"ca.pem"),FLOWSQL_KAFKA_KEYTAB=str(kerberos/"alice.keytab"),FLOWSQL_KAFKA_REPEAT_SECONDS="35",FLOWSQL_KAFKA_REPEAT_EXPECT_FAILURE="1")
  renewal=subprocess.Popen(client_command(renewenv),env=renewenv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  children.append(renewal)
  initial=renewal.stdout.readline()
  assert json.loads(initial)["status"]=="passed",mechanism+" initial auth"
  if mechanism=="OAUTHBEARER":idp_failing=True
  else:kdc.terminate();kdc.wait(timeout=10)
  stdout,stderr=renewal.communicate(timeout=60)
  (root/(mechanism+"-renewal-failure.log")).write_text(initial+stdout+stderr)
  assert renewal.returncode==0 and json.loads(stdout.splitlines()[0])["status"]=="failed",mechanism+" renewal must fail"
  results.append([mechanism,"renewal failure","passed"]);print(mechanism,"renewal failure passed",flush=True)
  idp_failing=False
 assert len(token_requests)>=2,"OAuth token must refresh before expiry"
 results.append(["OAuth HTTPS refresh",len(token_requests),"passed"])
 (root/"results.json").write_text(json.dumps(results,indent=2))
finally:
 process.terminate()
 process.wait(timeout=30);log.close();kdc.terminate();kdc.wait(timeout=10);kdc_log.close();idp.shutdown()
