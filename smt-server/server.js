// Stellarium Web - Copyright (c) 2020 - Stellarium Labs SAS
//
// This program is licensed under the terms of the GNU AGPL v3, or
// alternatively under a commercial licence.
//
// The terms of the AGPL v3 license can be found in the main directory of this
// repository.
//
// This file is part of the Survey Monitoring Tool plugin, which received
// funding from the Centre national d'études spatiales (CNES).

import express from 'express'
import cors from 'cors'
import fsp from 'fs/promises'
import fs from 'fs'
import _ from 'lodash'
import QueryEngine from './query-engine.mjs'
import bodyParser from 'body-parser'
import NodeGit from 'nodegit'
import hash_sum from 'hash-sum'

const SMT_SERVER_INFO = {
  version: process.env.npm_package_version || 'dev',
  dataGitServer: 'git@github.com:Stellarium-Labs/smt-data.git',
  dataGitBranch: process.env.SMT_DATA_BRANCH || 'master',
  dataGitSha1: '',
  dataLocalModifications: undefined,
  baseHashKey: ''
}

let status = 'starting'

console.log('Starting SMT Server ' + SMT_SERVER_INFO.version + ' on data branch ' + SMT_SERVER_INFO.dataGitBranch)

const app = express()
app.use(cors())
app.use(bodyParser.json())         // to support JSON-encoded bodies

// Allow to catch CTRL+C when runnning inside a docker
process.on('SIGINT', () => {
  console.info("User Interrupted")
  process.exit(0)
})

const port = process.env.PORT || 8100
const __dirname = process.cwd()

// Start listening to connection even during startup
app.listen(port, () => {
  console.log(`SMT Server listening at http://localhost:${port}`)
})

app.get('/api/v1/status', (req, res) => {
  res.send({status: status})
})

const syncGitData = async function (gitServer, gitBranch) {
  const localPath = __dirname + '/data'
  const cloneOptions = {
    fetchOpts: {
      callbacks: {
        certificateCheck: function() { return 0 },
        credentials: function(url, userName) {
          if (fs.existsSync(__dirname + '/access_key.pub')) {
            return NodeGit.Cred.sshKeyNew(
              userName,
              __dirname + '/access_key.pub',
              __dirname + '/access_key', '')
          } else {
            return NodeGit.Cred.sshKeyFromAgent(userName)
          }
        }
      }
    }
  }
  console.log('Synchronizing with SMT data git repo: ' + gitServer)
  const repo = await NodeGit.Clone(gitServer, localPath, cloneOptions)
    .catch(err => {
      console.log('Repo already exists, use local version from ' + localPath)
      return NodeGit.Repository.open(localPath)
    })
  await repo.fetchAll(cloneOptions.fetchOpts)
  console.log('Getting to last commit on branch ' + gitBranch)
  const ref = await repo.getBranch('refs/remotes/origin/' + gitBranch)
  await repo.checkoutRef(ref)
  const commit = await repo.getHeadCommit()
  const statuses = await repo.getStatus()
  const ret = {}
  ret.dataGitSha1 = await commit.sha()
  ret.modified = false
  statuses.forEach(s => { if (s.isModified()) ret.modified = true })
  if (ret.modified) console.log('Data has local modifications')
  return ret
}

const getSmtServerSourceCodeHash = async function () {
  let extraVersionHash = ''
  try {
    extraVersionHash = await fsp.readFile(__dirname + '/extraVersionHash.txt', 'utf-8')
    extraVersionHash = extraVersionHash.trim()
  } catch (err) {
    console.log('No extraVersionHash.txt file found, try to generate one from git status')
    // Check if this server is in a git and if it has modifications, generate
    // an extraVersionHash on the fly
    try {
      const repo = await NodeGit.Repository.open(__dirname + '/..')
      const commit = await repo.getHeadCommit()
      const statuses = await repo.getStatus()
      const serverCodeGitSha1 = await commit.sha()
      let modified = false
      statuses.forEach(s => { if (s.isModified()) modified = true })
      extraVersionHash = serverCodeGitSha1
      if (modified) {
        console.log('Server code has local modifications')
        extraVersionHash += '_' + Date.now()
      }
    } catch (err) {
      // This server is not in a git, just give up and return empty string
    }
  }
  return extraVersionHash
}

const smtServerSourceCodeHash = await getSmtServerSourceCodeHash()

status = 'syncing data'
const ret = await syncGitData(SMT_SERVER_INFO.dataGitServer, SMT_SERVER_INFO.dataGitBranch)
SMT_SERVER_INFO.dataGitSha1 = ret.dataGitSha1
SMT_SERVER_INFO.dataLocalModifications = ret.modified

// Compute the base hash key which is unique for a given version of the server
// code and data. It will be used to generate cache-friendly URLs.
let baseHashKey = SMT_SERVER_INFO.dataGitSha1 + smtServerSourceCodeHash
if (SMT_SERVER_INFO.dataLocalModifications)
  baseHashKey += '_' + Date.now()
SMT_SERVER_INFO.baseHashKey = hash_sum(baseHashKey)
console.log('Server base hash key: ' + SMT_SERVER_INFO.baseHashKey)

status = 'loading data'
const dbFileName = __dirname + '/qe.db'
// Check if we can preserve the previous DB to avoid re-loading the whole DB
let reloadGeojson = true
if (fs.existsSync(dbFileName) && !fs.existsSync('dontReloadGeojson')) {
  try {
    const dbServerInfo = QueryEngine.getDbExtraInfo(dbFileName)
    if (dbServerInfo.baseHashKey === SMT_SERVER_INFO.baseHashKey) {
      reloadGeojson = false
    }
  } catch (err) {}
}

if (reloadGeojson) {
  console.log('Data or code has changed since last start: reload geojson')
  await QueryEngine.generateDb(__dirname + '/data/', dbFileName, SMT_SERVER_INFO)
  console.log('*** DB Loading finished ***')
} else {
  console.log('No data/code change since last start: reload previous DB')
}

// Initialize the read-only engine
const qe = new QueryEngine(dbFileName)
status = 'ready'

// Global storage of hash -> query for later lookup
const hashToQuery = {}

// Insert this query in the global list of hash -> query for later lookup
// Returns a unique hash key referencing this query
const insertQuery = function (q) {
  // Inject a key unique to each revision of the input data
  // this ensure the hash depends on query + data content
  q.baseHashKey = SMT_SERVER_INFO.baseHashKey
  const hash = hash_sum(q)
  hashToQuery[hash] = q
  return hash
}

// Returns the query matching this hash key
const lookupQuery = function (hash) {
  if (!(hash in hashToQuery))
    return undefined
  return _.cloneDeep(hashToQuery[hash])
}

app.get('/api/v1/smtServerInfo', (req, res) => {
  res.send(qe.extraInfo)
})

app.get('/api/v1/smtConfig', (req, res) => {
  res.send(qe.smtConfig)
})

app.get('/api/v1/:serverHash/query', async (req, res) => {
  if (req.params.serverHash !== SMT_SERVER_INFO.baseHashKey) {
    res.status(404).send()
    return
  }
  const q = JSON.parse(decodeURIComponent(req.query.q))
  res.set('Cache-Control', 'public, max-age=31536000')
  const queryResp = await qe.queryAsync(q)
  res.send(queryResp)
})

app.get('/api/v1/:serverHash/queryVisual', (req, res) => {
  if (req.params.serverHash !== SMT_SERVER_INFO.baseHashKey) {
    res.status(404).send()
    return
  }
  const q = JSON.parse(decodeURIComponent(req.query.q))
  res.set('Cache-Control', 'public, max-age=31536000')
  res.send(insertQuery(q))
})

app.get('/api/v1/hips/:queryHash/properties', (req, res) => {
  if (!lookupQuery(req.params.queryHash)) {
    res.status(404).send()
    return
  }

  res.set('Cache-Control', 'public, max-age=31536000')
  res.type('text/plain')
  res.send(qe.getHipsProperties())
})

app.get('/api/v1/hips/:queryHash/:order(Norder\\d+)/:dir/:pix.geojson', async (req, res) => {
  res.set('Cache-Control', 'public, max-age=31536000')

  const order = parseInt(req.params.order.replace('Norder', ''))
  const pix = parseInt(req.params.pix.replace('Npix', ''))
  const q = lookupQuery(req.params.queryHash)
  if (!q) {
    res.status(404).send()
    return
  }
  const tileResp = await qe.getHipsTileAsync(q, order, pix)
  if (!tileResp) {
    res.status(404).send()
    return
  }
  res.send(tileResp)
})

app.get('/api/v1/hips/:queryHash/Allsky.geojson', async (req, res) => {
  res.set('Cache-Control', 'public, max-age=31536000')
  const q = lookupQuery(req.params.queryHash)
  if (!q) {
    res.status(404).send()
    return
  }
  const tileResp = await qe.getHipsTileAsync(q, -1, 0)
  if (!tileResp) {
    res.status(404).send()
    return
  }
  res.send(tileResp)
})

