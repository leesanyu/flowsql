# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Exercise real build recipe failure paths and cross-process cache exclusion without touching the installed cache."""
import pathlib, tempfile, subprocess, json, os, hashlib, concurrent.futures
repo=pathlib.Path(__file__).resolve().parents[3]
(repo/"evidence/kafka-connection-auth").mkdir(parents=True,exist_ok=True)
root=pathlib.Path(tempfile.mkdtemp(prefix="dependency-fixture-",dir=repo/"evidence/kafka-connection-auth"))
recipe=repo/"thirdparts/librdkafka/build-client.cmake"
def invoke(values):
 command=["cmake"]+["-D"+k+"="+str(v) for k,v in values.items()]+["-P",str(recipe)]
 return subprocess.run(command,capture_output=True,text=True,timeout=60)
def expect_failure(label,values,marker):
 r=invoke(values);(root/(label+".log")).write_text(r.stdout+r.stderr)
 assert r.returncode and marker in r.stdout+r.stderr,label
base={"PREFIX":root/"prefix","INSTALL":root/"install","FINGERPRINT":"fixture-v1","OFFLINE":"ON","ARCHIVE":root/"absent.tar.gz"}
expect_failure("offline-missing",base,"offline build requires")
corrupt=root/"corrupt.tar.gz";corrupt.write_bytes(b"bad archive")
expect_failure("checksum",dict(base,ARCHIVE=corrupt,SHA256="0"*64),"checksum mismatch")
(root/"install/lib").mkdir(parents=True)
(root/"install/lib/librdkafka.a").write_bytes(b"fixture")
(root/"install/flowsql-fingerprint").write_text("another-fingerprint")
expect_failure("fingerprint",base,"fingerprint mismatch")
# A matching complete fixture cache is shared by concurrent processes even without an archive/network.
(root/"install/flowsql-fingerprint").write_text("fixture-v1")
with concurrent.futures.ThreadPoolExecutor(2) as executor:
 outcomes=list(executor.map(lambda _:invoke(base),range(2)))
assert all(r.returncode==0 for r in outcomes),"matching concurrent reuse"
# A separate fingerprint with source-path retained but required SSL macro absent must be rejected.
missing=root/"missing";(missing/"build/generated").mkdir(parents=True);source=missing/"source";source.mkdir()
(source/"CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.12)\nproject(feature_fixture C)\nfile(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")\nfile(WRITE "${CMAKE_BINARY_DIR}/generated/config.h" "#define WITH_SSL 0\\n")\n')
(missing/"source-path").write_text(str(source));archive=root/"archive";archive.write_bytes(b"source placeholder")
expect_failure("feature",dict(base,PREFIX=missing,INSTALL=root/"missing-install",ARCHIVE=archive,SHA256=hashlib.sha256(archive.read_bytes()).hexdigest(),C_COMPILER="/usr/bin/cc",CXX_COMPILER="/usr/bin/c++"),"required feature is absent: WITH_SSL")
print("PASS: offline missing cache, SHA256 mismatch, fingerprint mismatch, concurrent reuse and missing feature rejection")
print("fixture evidence:",root)
