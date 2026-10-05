SELECT * FROM netadapter.eth0 USING npm.basic
WITH features='basic,session,dns,http1,tls,icmp,labeling',
     parameters='{"schema_version":1,"core":{"labeling":"config.capture-protocols@1"},"dns":{"primary_label_ids":[1001]},"http1":{"primary_label_ids":[1002]},"tls":{"primary_label_ids":[1003]}}'
INTO mysql.flowsql-mysql
