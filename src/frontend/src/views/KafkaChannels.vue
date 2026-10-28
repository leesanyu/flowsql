<template>
  <div class="kafka-channels" v-loading="loading">
    <el-alert title="Kafka 支持结构化与原始包接入/发布；原始包发布使用异步 SELECT * FROM netadapter.通道 INTO kafka.通道。连接验证只检查认证和元数据，读写权限由实际任务验证。" type="info" :closable="false" />
    <div class="toolbar">
      <el-button type="primary" @click="openChannel()">新建 Kafka 通道</el-button>
      <el-button @click="openConnection()">新建连接配置</el-button>
      <el-button @click="load">刷新列表</el-button>
      <span v-if="error" role="alert">{{ error }}</span>
    </div>
    <el-tabs v-model="tab">
      <el-tab-pane label="通道" name="channels" />
      <el-tab-pane label="连接配置" name="connections" />
    </el-tabs>
    <el-table :data="filteredRows" row-key="name">
      <el-table-column prop="name" label="名称" min-width="140" />
      <el-table-column v-if="tab === 'channels'" prop="role" label="角色" width="90" />
      <el-table-column v-if="tab === 'channels'" prop="topic" label="Topic" min-width="130" />
      <el-table-column v-if="tab === 'channels'" prop="connection_name" label="连接" min-width="130" />
      <el-table-column prop="security_protocol" label="协议" min-width="140" />
      <el-table-column prop="sasl_mechanism" label="SASL 机制" min-width="140" />
      <el-table-column label="配置" min-width="130">
        <template #default="{ row }">已保存 · r{{ row.revision }}<el-tag v-if="row.in_use" type="warning">使用中</el-tag></template>
      </el-table-column>
      <el-table-column label="连接验证" min-width="210"><template #default="{ row }">{{ verificationLabel(row) }}</template></el-table-column>
      <el-table-column v-if="tab === 'channels'" label="数据能力" min-width="180">
        <template #default="{ row }"><div v-for="(state, direction) in row.adapter_status" :key="direction">{{ direction }}：{{ ({ not_installed: '适配器未安装', configuration_required: '待配置', ready: '已就绪' })[state] || state }}</div></template>
      </el-table-column>
      <el-table-column label="操作" min-width="350">
        <template #default="{ row }">
          <el-button text @click="detail = row">详情</el-button>
          <el-button v-if="tab === 'channels' && row.payload_kind === 'structured' && row.role !== 'sink'" text @click="dictionary(row)">数据字典</el-button>
          <el-button text :disabled="!!blockedReason(row, tab)" :title="blockedReason(row, tab)" @click="tab === 'channels' ? openChannel(row) : openConnection(row)">编辑</el-button>
          <el-button text :disabled="testing" @click="verifySaved(row)">验证</el-button>
          <el-button v-if="tab === 'channels'" text @click="copyReference(row)">复制引用</el-button>
          <el-button text type="danger" :disabled="!!blockedReason(row, tab, true)" :title="blockedReason(row, tab, true)" @click="remove(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>
    <el-dialog v-model="connectionVisible" :title="connectionRevision ? '编辑连接配置' : '新建连接配置'" width="750px" :close-on-click-modal="false" @closed="clearConnection">
      <el-form label-width="145px">
        <el-form-item label="名称" required><el-input v-model.trim="connection.name" :disabled="!!connectionRevision" /></el-form-item>
        <el-form-item v-if="connectionRevision" label="配置 revision">r{{ connectionRevision }}（只读）</el-form-item>
        <el-form-item label="Bootstrap servers" required><el-input v-model="connection.servers" type="textarea" placeholder="host:9092，每行一个或用逗号分隔" /></el-form-item>
        <el-form-item label="安全协议"><el-select v-model="connection.security_protocol"><el-option v-for="p in definitions.security_protocols || []" :key="p" :label="p" :value="p" /></el-select></el-form-item>
        <template v-if="usesTls(connection.security_protocol)">
          <el-form-item v-for="field in ['ca_ref', 'certificate_ref', 'private_key_ref']" :key="field" :label="labels[field]">
            <el-select v-model="connection.tls[field]" clearable filterable placeholder="可选；CA 留空使用系统信任"><el-option v-for="ref in fileAliases" :key="ref" :label="ref" :value="ref" /></el-select>
          </el-form-item>
          <p class="hint">证书链和主机名始终校验；客户端证书与私钥成对选择。</p>
        </template>
        <template v-if="usesSasl(connection.security_protocol)">
          <el-form-item label="SASL 机制"><el-select v-model="connection.sasl_mechanism"><el-option v-for="m in definitions.sasl_mechanisms || []" :key="m.name" :value="m.name" :label="m.available ? m.name : `${m.name}（${m.reason}）`" :disabled="!m.available" /></el-select></el-form-item>
          <el-form-item v-for="field in activeSaslFields" :key="field" :label="labels[field] || field">
            <el-select v-if="field.endsWith('_ref')" v-model="connection.sasl[field]" clearable filterable><el-option v-for="ref in fileAliases" :key="ref" :value="ref" :label="ref" /></el-select>
            <el-input v-else v-model="connection.sasl[field]" :placeholder="field === 'token_endpoint' ? 'https://idp.example/token' : ''" />
          </el-form-item>
        </template>
        <div v-for="field in secretFields(connection)" :key="field" class="secret">
          <el-form-item :label="labels[field]">
            <span>{{ connection.secrets[field].configured ? '已配置（不回显）' : '未配置' }}</span>
            <el-select v-model="connection.secrets[field].operation">
              <el-option label="保留原值" value="keep" /><el-option label="替换" value="replace" /><el-option label="清除" value="clear" :disabled="field !== 'private_key_password'" />
            </el-select>
          </el-form-item>
          <el-form-item v-if="connection.secrets[field].operation === 'replace'" label="新凭证">
            <el-select v-model="connection.secrets[field].mode"><el-option label="输入新值" value="value" /><el-option label="使用部署凭证引用" value="reference" /></el-select>
            <el-input v-if="connection.secrets[field].mode === 'value'" v-model="connection.secrets[field].value" type="password" show-password autocomplete="new-password" />
            <el-select v-else v-model="connection.secrets[field].reference" filterable><el-option v-for="ref in secretAliases" :key="ref" :value="ref" :label="ref" /></el-select>
          </el-form-item>
        </div>
      </el-form>
      <p v-if="error" role="alert">{{ error }}</p>
      <template #footer>
        <el-button @click="closeConnection">取消</el-button>
        <el-button :disabled="testing" @click="verifyDraft('connection')">验证草稿</el-button>
        <el-button type="primary" :loading="saving" @click="saveConnection">保存连接</el-button>
      </template>
    </el-dialog>
    <el-dialog v-model="channelVisible" :title="channelRevision ? '编辑 Kafka 通道' : '新建 Kafka 通道'" width="680px" :close-on-click-modal="false" @closed="clearChannel">
      <el-form label-width="120px">
        <el-form-item label="通道名称" required><el-input v-model.trim="channel.name" :disabled="!!channelRevision" /></el-form-item>
        <el-form-item v-if="channelRevision" label="配置 revision">r{{ channelRevision }}（只读）</el-form-item>
        <el-form-item label="连接配置" required><el-select v-model="channel.connection_name" filterable><el-option v-for="c in connections" :key="c.name" :value="c.name" :label="c.name" /></el-select><el-button text @click="openConnection()">就地新建连接</el-button></el-form-item>
        <el-form-item label="角色"><el-select v-model="channel.role"><el-option v-for="role in definitions.roles || []" :key="role" :value="role" :label="role" /></el-select></el-form-item>
        <el-form-item label="Topic" required><el-input v-model="channel.topic" /></el-form-item>
        <el-form-item v-if="channel.role !== 'sink'" label="消费组" required><el-input v-model="channel.consumer_group" /></el-form-item>
        <el-form-item label="数据类别"><el-select v-model="channel.payload_kind"><el-option label="结构化数据" value="structured" /><el-option label="原始 packet" value="packet" /></el-select></el-form-item>
        <template v-if="channel.role !== 'sink' && channel.payload_kind === 'packet'">
          <el-form-item label="原始包接入"><el-select v-model="channel.packet_source_enabled"><el-option label="配置 flowsql_packet_v1" :value="true" /><el-option label="暂不配置" :value="false" /></el-select></el-form-item>
          <template v-if="channel.packet_source_enabled">
            <p class="hint">Topic 须预建为单分区并使用 delete 保留策略；一个观测域共用一个活动发布者。每次分析重启建立新的 run，位点续读不保留旧会话内存。</p>
            <el-form-item label="stream_id" required><el-input v-model.trim="channel.packet_source.stream_id" /></el-form-item>
            <el-form-item label="观测域 ID" required><el-input v-model.trim="channel.packet_source.observation_domain_id" placeholder="非零 uint64 十进制字符串" /></el-form-item>
            <div v-for="(input, index) in channel.packet_source.inputs" :key="index" class="secret">
              <el-form-item :label="`输入 ${index + 1}`"><el-input v-model="input.source_name" placeholder="探针 Capture V2 输入名称" /><el-button text type="danger" @click="channel.packet_source.inputs.splice(index, 1)">删除输入</el-button></el-form-item>
              <el-form-item label="source_id"><el-input-number v-model="input.source_id" :min="0" :max="4294967295" :precision="0" /></el-form-item>
              <el-form-item label="queue_id"><el-input-number v-model="input.queue_id" :min="0" :max="4294967295" :precision="0" /></el-form-item>
            </div>
            <el-button :disabled="channel.packet_source.inputs.length >= 32" @click="channel.packet_source.inputs.push({ source_id: channel.packet_source.inputs.length, queue_id: 0, source_name: '' })">新增采集输入</el-button>
            <el-form-item label="无有效位点"><el-select v-model="channel.packet_source.offset_reset"><el-option label="从最早开始" value="earliest" /><el-option label="从最新开始" value="latest" /><el-option label="明确报错" value="error" /></el-select></el-form-item>
            <el-form-item label="包确认策略"><el-select v-model="channel.packet_source.commit_policy"><el-option label="处理、事实和当次输出成功后确认" value="after_success" /><el-option label="不提交位点" value="none" /></el-select></el-form-item>
            <el-form-item label="批次包数上限"><el-input-number v-model="channel.packet_source.max_packets_per_batch" :min="1" :max="4096" :precision="0" /></el-form-item>
            <el-form-item label="原始字节上限"><el-input-number v-model="channel.packet_source.max_bytes_per_batch" :min="1024" :max="16777216" :precision="0" /></el-form-item>
            <el-form-item label="包消息字节上限"><el-input-number v-model="channel.packet_source.max_message_bytes" :min="1024" :max="16777216" :precision="0" /></el-form-item>
            <el-form-item label="最大等待毫秒"><el-input-number v-model="channel.packet_source.max_wait_ms" :min="1" :max="1000" :precision="0" /></el-form-item>
          </template>
        </template>
        <template v-if="channel.role !== 'source' && channel.payload_kind === 'packet'">
          <el-form-item label="原始包发布"><el-select v-model="channel.packet_sink_enabled"><el-option label="配置 flowsql_packet_v1 发布" :value="true" /><el-option label="暂不配置" :value="false" /></el-select></el-form-item>
          <template v-if="channel.packet_sink_enabled">
            <el-form-item label="发布 stream_id" required><el-input v-model.trim="channel.packet_sink.stream_id" /></el-form-item>
            <el-form-item label="发布消息字节上限"><el-input-number v-model="channel.packet_sink.max_message_bytes" :min="1024" :max="16777216" :precision="0" /></el-form-item>
            <el-form-item label="发布队列条数上限"><el-input-number v-model="channel.packet_sink.queue_max_messages" :min="1" :max="65536" :precision="0" /></el-form-item>
            <el-form-item label="发布队列字节上限"><el-input-number v-model="channel.packet_sink.queue_max_bytes" :min="1048576" :max="268435456" :precision="0" /></el-form-item>
            <el-form-item label="发布确认预算毫秒"><el-input-number v-model="channel.packet_sink.delivery_timeout_ms" :min="100" :max="120000" :precision="0" /></el-form-item>
            <p class="hint">来源网卡在 netadapter 通道中配置。预建 delete 单分区 Topic，并保证同 Topic/stream_id 只有一个发布者；仅支持 SELECT *，不带 WHERE 或 USING。一个采集 poll 完整发送并等待确认，阻塞时暂停拉取，网卡仍可能丢包。</p>
            <p class="hint">任务摘要提供观测域与输入映射，可用于接收端 packet source 配置。确认表示 broker 已确认消息，未知发送结果不会自动重放；采集丢包仅依据采集计数器。</p>
          </template>
        </template>
        <template v-if="channel.role !== 'sink' && channel.payload_kind === 'structured'">
          <el-form-item label="结构化接入"><el-select v-model="channel.source_enabled"><el-option label="配置 JSON 字段" :value="true" /><el-option label="暂不配置" :value="false" /></el-select></el-form-item>
          <template v-if="channel.source_enabled">
            <el-form-item label="实体名称"><el-input v-model.trim="channel.source.entity" /></el-form-item>
            <p class="hint">每条消息为一个 JSON 对象。路径使用 JSON Pointer；时间字段使用 int64，单位填写 unix_s / unix_ms / unix_us / unix_ns。非法消息终止任务并保留重放机会。</p>
            <div v-for="(field, index) in channel.source.fields" :key="index" class="secret">
              <el-form-item :label="`字段 ${index + 1}`"><el-input v-model.trim="field.name" placeholder="输出列名" /><el-button text type="danger" @click="channel.source.fields.splice(index, 1)">删除字段</el-button></el-form-item>
              <el-form-item label="JSON 路径"><el-input v-model="field.path" placeholder="/value 或 /nested/value" /></el-form-item>
              <el-form-item label="字段类型"><el-select v-model="field.type"><el-option v-for="type in ['bool', 'int64', 'uint64', 'float64', 'utf8']" :key="type" :value="type" :label="type" /></el-select></el-form-item>
              <el-form-item label="允许空值"><el-select v-model="field.nullable"><el-option label="允许缺失或 null" :value="true" /><el-option label="必须存在且非 null" :value="false" /></el-select></el-form-item>
              <el-form-item label="单位"><el-input v-model.trim="field.unit" placeholder="可选，如 bytes、unix_ms" /></el-form-item>
            </div>
            <el-button :disabled="channel.source.fields.length >= 128" @click="channel.source.fields.push({ name: '', path: '/', type: 'utf8', nullable: false, unit: '' })">新增字段</el-button>
            <el-form-item label="无有效位点"><el-select v-model="channel.source.offset_reset"><el-option label="从最早开始" value="earliest" /><el-option label="从最新开始" value="latest" /><el-option label="明确报错" value="error" /></el-select></el-form-item>
            <el-form-item label="确认策略"><el-select v-model="channel.source.commit_policy"><el-option label="当前批次处理及结果投递成功后确认" value="after_success" /><el-option label="不提交位点" value="none" /></el-select></el-form-item>
            <el-form-item label="批次最多行数"><el-input-number v-model="channel.source.max_batch_rows" :min="1" :max="4096" /></el-form-item>
            <el-form-item label="单消息字节上限"><el-input-number v-model="channel.source.max_message_bytes" :min="1" :max="16777216" /></el-form-item>
          </template>
        </template>
        <template v-if="channel.role !== 'source' && channel.payload_kind === 'structured'">
          <el-form-item label="结果发布"><el-select v-model="channel.sink_enabled"><el-option label="配置 JSON 结果信封" :value="true" /><el-option label="暂不配置" :value="false" /></el-select></el-form-item>
          <template v-if="channel.sink_enabled">
            <el-form-item label="发布实体" required><el-input v-model.trim="channel.sink.entity" placeholder="如 alert_results" /></el-form-item>
            <el-form-item label="Schema 版本" required><el-input-number v-model="channel.sink.schema_version" :min="1" :max="2147483647" :precision="0" /></el-form-item>
            <el-form-item label="记录语义"><el-select v-model="channel.sink.record_kind"><el-option label="event 独立事件" value="event" /><el-option label="upsert 完整替换快照" value="upsert" /></el-select></el-form-item>
            <el-form-item label="业务 key 列"><el-input v-model="channel.sink.key_fields" placeholder="按顺序逗号分隔，最多 8 列" /></el-form-item>
            <el-form-item v-if="channel.sink.record_kind === 'upsert'" label="revision 列" required><el-input v-model.trim="channel.sink.revision_field" /></el-form-item>
            <p class="hint">upsert 的 key 不含 revision；revision 必须由结果提供，接收方按 key/revision 处理重复或乱序。event 不填写 key 时，重执行会产生新的身份。每行一条 JSON，无结束消息。</p>
            <el-form-item label="消息字节上限"><el-input-number v-model="channel.sink.max_message_bytes" :min="1024" :max="16777216" :precision="0" /></el-form-item>
            <el-form-item label="队列条数上限"><el-input-number v-model="channel.sink.queue_max_messages" :min="1" :max="65536" :precision="0" /></el-form-item>
            <el-form-item label="队列字节上限"><el-input-number v-model="channel.sink.queue_max_bytes" :min="1048576" :max="268435456" :precision="0" /></el-form-item>
            <el-form-item label="确认预算毫秒"><el-input-number v-model="channel.sink.delivery_timeout_ms" :min="100" :max="120000" :precision="0" /></el-form-item>
            <p class="hint">发布完成表示 broker 已确认全部消息。失败或取消可能已有部分发布，摘要保留确认、未发送和未知数量；不自动重放。</p>
          </template>
        </template>
      </el-form>
      <el-alert title="位点确认覆盖当前批次的处理和结果投递；有状态算子的最终落盘与 Kafka 位点不构成同一事务，重放可能产生重复结果。" type="info" :closable="false" />
      <p v-if="error" role="alert">{{ error }}</p>
      <template #footer><el-button @click="closeChannel">取消</el-button><el-button :disabled="testing" @click="verifyDraft('channel')">验证草稿</el-button><el-button type="primary" :loading="saving" @click="saveChannel">保存通道</el-button></template>
    </el-dialog>
    <el-drawer :model-value="!!detail" title="Kafka 配置详情" size="60%" @close="detail = null">
      <template v-if="detail"><p v-if="detail.references?.length">引用通道：{{ detail.references.join('、') }}</p><pre>{{ JSON.stringify(detail, null, 2) }}</pre></template>
    </el-drawer>
    <div class="verification" role="status">
      <el-input-number v-model="timeoutSeconds" :min="1" :max="30" :precision="0" /><span>验证预算（秒）</span>
      <span v-if="testing">连接验证进行中 · {{ testResult?.stage || 'network' }}</span>
      <el-button v-if="testing" @click="cancelTest">取消验证</el-button>
      <div v-if="testResult && !testing">{{ testResult.status === 'passed' ? '连接验证通过' : '验证失败' }} · {{ testResult.stage }} · {{ testResult.code }}<br />读权限未验证，写权限未验证。<span v-if="testResult.checked_brokers?.length">已检查 broker：{{ testResult.checked_brokers.join('、') }}</span></div>
    </div>
  </div>
</template>
<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import api from '../api'
import { connectionDraft, channelDraft, buildConnection, buildChannel, usesTls, usesSasl, secretFields, blockedReason, kafkaError, verificationLabel } from '../utils/kafkaChannel.js'
const router = useRouter()
function dictionary(row) { router.push({ path: '/semantics', query: { dataset: JSON.stringify({ kind: 'kafka_source', name: row.name }) } }) }
const props = defineProps({ searchText: { type: String, default: '' } })
const emit = defineEmits(['count-change'])
const tab = ref('channels'), connections = ref([]), channels = ref([]), definitions = ref({})
const loading = ref(false), saving = ref(false), error = ref(''), detail = ref(null)
const connection = ref(connectionDraft()), channel = ref(channelDraft())
const connectionVisible = ref(false), channelVisible = ref(false), connectionRevision = ref(0), channelRevision = ref(0)
const timeoutSeconds = ref(10), testing = ref(false), testId = ref(''), testResult = ref(null)
let alive = true, cancelRequested = false, timer = null
const labels = { ca_ref: 'CA 信任引用', certificate_ref: '客户端证书', private_key_ref: '客户端私钥', username: '用户名', principal: 'Kerberos principal', service_name: '服务名', keytab_ref: 'Keytab 引用', token_endpoint: 'Token HTTPS 地址', client_id: 'OAuth client_id', scope: 'OAuth scope（可选）', password: '认证密码', client_secret: 'OAuth client_secret', private_key_password: '私钥口令（可选）' }
const fileAliases = computed(() => definitions.value.credential_aliases?.files || [])
const secretAliases = computed(() => [...fileAliases.value, ...(definitions.value.credential_aliases?.environments || [])])
const activeSaslFields = computed(() => (definitions.value.mechanism_fields?.[connection.value.sasl_mechanism] || []).filter((key) => !['password', 'client_secret'].includes(key)))
const filteredRows = computed(() => (tab.value === 'connections' ? connections.value : channels.value).filter((row) => row.name.includes(props.searchText.trim())))
const report = (failure) => { error.value = kafkaError(failure); ElMessage.error(error.value) }
const all = async (kind) => {
  const rows = []; let after = ''
  do { const { data } = await api.kafkaQuery(kind, after); rows.push(...data.items); after = data.next_cursor || '' } while (after && alive)
  return rows
}
const load = async () => {
  loading.value = true
  try { const [d, c, ch] = await Promise.all([api.kafkaDefinitions(), all('connections'), all('channels')]); if (alive) { definitions.value = d.data; connections.value = c; channels.value = ch; emit('count-change', ch.length) } } catch (failure) { if (alive) report(failure) } finally { loading.value = false }
}
const clearConnection = () => { connection.value = connectionDraft(); connectionRevision.value = 0 }
const clearChannel = () => { channel.value = channelDraft(); channelRevision.value = 0 }
const openConnection = (row) => { error.value = ''; connection.value = connectionDraft(row); connectionRevision.value = row?.revision || 0; connectionVisible.value = true }
const openChannel = (row) => { error.value = ''; channel.value = channelDraft(row); channelRevision.value = row?.revision || 0; channelVisible.value = true }
const cancelTest = async () => { cancelRequested = true; if (testId.value) { try { await api.kafkaTestCancel(testId.value) } catch (failure) { if (alive) report(failure) } } }
const closeConnection = () => { void cancelTest(); connectionVisible.value = false; clearConnection() }
const closeChannel = () => { void cancelTest(); channelVisible.value = false; clearChannel() }
const saveConnection = async () => {
  try { const payload = buildConnection(connection.value, connectionRevision.value); saving.value = true; await api.kafkaSave('connections', payload, !!connectionRevision.value); if (channelVisible.value) channel.value.connection_name = payload.name; closeConnection(); await load(); ElMessage.success('连接配置已保存；需要单独验证连接') } catch (failure) { report(failure) } finally { saving.value = false }
}
const saveChannel = async () => {
  try { const payload = buildChannel(channel.value, channelRevision.value); saving.value = true; await api.kafkaSave('channels', payload, !!channelRevision.value); closeChannel(); await load(); ElMessage.success('通道配置已保存；数据能力按适配器状态判断') } catch (failure) { report(failure) } finally { saving.value = false }
}
const remove = async (row) => {
  if (blockedReason(row, tab.value, true)) return
  const kind = tab.value
  try { await ElMessageBox.confirm(`确认删除 ${row.name}？`, '删除确认', { type: 'warning' }); await api.kafkaRemove(kind, row); await load() } catch (failure) { if (failure !== 'cancel' && failure !== 'close') report(failure) }
}
const copyReference = async (row) => { try { await navigator.clipboard.writeText(`kafka.${row.name}`); ElMessage.success('已复制通道引用') } catch { ElMessage.warning('请手动复制 kafka.' + row.name) } }
const poll = async () => {
  try { const { data } = await api.kafkaTestQuery(testId.value); if (!alive) return; testResult.value = data; if (data.status === 'running') timer = setTimeout(poll, 250); else { testing.value = false; testId.value = ''; await load() } } catch (failure) { if (alive) { report(failure); void cancelTest(); testing.value = false } }
}
const startTest = async (payload) => {
  if (testing.value) return
  testing.value = true; cancelRequested = false; testResult.value = null; error.value = ''
  try { const { data } = await api.kafkaTestStart({ ...payload, timeout_ms: timeoutSeconds.value * 1000 }); testId.value = data.test_id; if (!alive || cancelRequested) await api.kafkaTestCancel(data.test_id); if (alive) { await load(); await poll() } } catch (failure) { if (alive) { testing.value = false; report(failure) } }
}
const verifySaved = (row) => startTest({ target: tab.value === 'connections' ? 'connection' : 'channel', name: row.name, expected_revision: row.revision })
const verifyDraft = (target) => {
  try { const draft = target === 'connection' ? buildConnection(connection.value, connectionRevision.value) : buildChannel(channel.value); return startTest({ target, draft }) } catch (failure) { report(failure) }
}
onMounted(load)
onBeforeUnmount(() => { alive = false; clearTimeout(timer); void cancelTest(); clearConnection(); clearChannel() })
</script>
<style scoped>
.toolbar, .verification { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin: 16px 0; }
.toolbar [role="alert"], [role="alert"] { color: var(--el-color-danger); }
.hint { color: var(--el-text-color-secondary); font-size: 13px; }
.secret { border-top: 1px solid var(--el-border-color); padding-top: 12px; }
.secret .el-input, .secret .el-select { margin-top: 6px; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; }
</style>
