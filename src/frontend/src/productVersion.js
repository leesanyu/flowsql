export function describeBuildInfo(info) {
  const revision = info.revision ? info.revision.slice(0, 12) : '未知'
  const dirty = info.dirty === null ? '未知' : info.dirty ? '有本地修改' : '无本地修改'
  return `提交：${revision}；源码状态：${dirty}`
}
