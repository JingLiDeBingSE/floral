/*
 * 网易云接口服务（NeteaseCloudMusicApi）启动器
 *
 * 端口 4000，和酷狗的 3000 并存。
 * 用法：node server.js
 */
const { serveNcmApi } = require('NeteaseCloudMusicApi')

const port = Number(process.env.PORT || 4000)

serveNcmApi({
  port,
  checkVersion: false,
  host: '127.0.0.1',
}).then((app) => {
  console.log(`netease api ready on http://127.0.0.1:${port}`)
}).catch((e) => {
  console.error('start failed:', e && e.message)
  process.exit(1)
})
