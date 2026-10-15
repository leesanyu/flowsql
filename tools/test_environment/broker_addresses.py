# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Read-only Metadata v1 for PLAINTEXT/TLS; authenticate SASL with the real client separately."""
import json
import socket
import ssl
import struct
import sys
import time

manifest, profile = sys.argv[1:]
settings=json.load(open(manifest))[profile]
connection=settings['connection']
protocol=connection['security_protocol']
if protocol not in ('PLAINTEXT','SSL'):
    print(json.dumps({'category':'not_applicable'}));raise SystemExit(0)
deadline=time.monotonic()+3
stage='bootstrap'

def endpoint(address):
    host,port=address.rsplit(':',1)
    return host.strip('[]'),int(port)

def connect(host,port):
    raw=socket.create_connection((host,port),timeout=max(.01,deadline-time.monotonic()))
    if protocol=='SSL':
        tls=connection['tls']
        context=ssl.create_default_context(cafile=settings['files'][tls['ca_ref'].split(':',1)[1]])
        if 'certificate_ref' in tls:
            context.load_cert_chain(settings['files'][tls['certificate_ref'].split(':',1)[1]],settings['files'][tls['private_key_ref'].split(':',1)[1]])
        try:return context.wrap_socket(raw,server_hostname=host)
        except BaseException:raw.close();raise
    return raw

def exact(stream,count):
    data=b''
    while len(data)<count:
        part=stream.recv(count-len(data))
        if not part:raise OSError('metadata response closed')
        data+=part
    return data

try:
    host,port=endpoint(connection['bootstrap_servers'][0])
    with connect(host,port) as stream:
        client=b'flowsql-environment'
        request=struct.pack('>hhih',3,1,7,len(client))+client+struct.pack('>i',0)
        stream.sendall(struct.pack('>i',len(request))+request)
        size=struct.unpack('>i',exact(stream,4))[0]
        if size<8 or size>16*1024*1024:raise ValueError('invalid metadata frame')
        body=exact(stream,size)
    offset=4
    count=struct.unpack_from('>i',body,offset)[0];offset+=4
    if not 0<count<10000:raise ValueError('invalid broker count')
    brokers=[]
    for _ in range(count):
        node=struct.unpack_from('>i',body,offset)[0];offset+=4
        length=struct.unpack_from('>h',body,offset)[0];offset+=2
        if length<=0:raise ValueError('invalid broker hostname')
        name=body[offset:offset+length].decode();offset+=length
        port=struct.unpack_from('>i',body,offset)[0];offset+=4
        rack=struct.unpack_from('>h',body,offset)[0];offset+=2
        if rack>0:offset+=rack
        brokers.append((name,port))
    stage='advertised'
    for host,port in brokers:
        with connect(host,port):pass
    print(json.dumps({'category':'ready','stage':'advertised','brokers':[f'{host}:{port}' for host,port in brokers],
                      'read_permission':'unverified','write_permission':'unverified'}))
except ssl.SSLError:
    print(json.dumps({'category':'tls_validation_failed','stage':stage}));raise SystemExit(1)
except PermissionError:
    print(json.dumps({'category':'execution_denied','stage':stage}));raise SystemExit(1)
except (OSError,ValueError,struct.error,UnicodeError):
    category='broker_address_unreachable' if stage=='advertised' else 'network_unreachable'
    print(json.dumps({'category':category,'stage':stage}));raise SystemExit(1)
