# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import sys, pathlib, subprocess, ctypes, json, os
binary=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/output/libflowsql_kafka.so').resolve()
result=subprocess.run(['ldd',str(binary)],capture_output=True,text=True,check=True)
assert 'not found' not in result.stdout and 'librdkafka.so' not in result.stdout
for soname in ['libssl.so.3','libcrypto.so.3','libcurl.so.4','libsasl2.so.2','libgssapi_krb5.so.2','libz.so.1','liblz4.so.1','libzstd.so.1']:
 assert soname in result.stdout,soname
 ctypes.CDLL(soname)
plugins=list(pathlib.Path('/usr/lib').glob('**/sasl2/libgssapiv2.so*'))
assert plugins,'Cyrus SASL GSSAPI module is missing'
ctypes.CDLL(str(plugins[0]))
assert os.access('/usr/bin/kinit',os.X_OK)
assert pathlib.Path('/etc/ssl/certs/ca-certificates.crt').stat().st_size>0
print(result.stdout)
print('PASS: private static Kafka client, shared authentication libraries, GSSAPI plugin, kinit and system CA')
