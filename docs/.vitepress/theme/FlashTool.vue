<script setup lang="ts">
import { onMounted, ref } from 'vue'

type ToolName = 'factory' | 'beta'
type ManifestImage = { name: string; file: string; address: number; size: number; sha256: string }
type Manifest = {
  version: string
  protocol: number
  secureVersion: number
  board: string
  images: ManifestImage[]
}

const UPDATE_PROTOCOL = 6
const RELEASE_API = 'https://api.github.com/repos/ZimengXiong/tinyTouch/releases?per_page=20'

const selected = ref<ToolName>('factory')
const manifest = ref<Manifest | null>(null)
const objectUrl = ref('')
const status = ref('')
const statusKind = ref('')

function show(message: string, kind = '') {
  status.value = message
  statusKind.value = kind
}

async function sha256(data: ArrayBuffer) {
  const digest = await crypto.subtle.digest('SHA-256', data)
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('')
}

function releaseAsset(file: string, tag?: string) {
  const query = new URLSearchParams({ file })
  if (tag) query.set('tag', tag)
  return `/api/github-release?${query}`
}

async function loadManifest(mode: ToolName) {
  let tag: string | undefined
  if (mode === 'beta') {
    const releasesResponse = await fetch(RELEASE_API, { cache: 'no-store' })
    if (!releasesResponse.ok) throw new Error('Beta releases could not be downloaded.')
    const releases = await releasesResponse.json() as Array<{ draft: boolean; prerelease: boolean; tag_name: string }>
    const beta = releases.find((release) =>
      !release.draft && release.prerelease && /^v[0-9]+\.[0-9]+\.[0-9]+-beta(?:[.-][0-9A-Za-z.-]+)?$/.test(release.tag_name)
    )
    if (!beta) throw new Error('No beta release is available.')
    tag = beta.tag_name
  }
  const label = mode === 'factory' ? 'Firmware' : 'Beta'
  const response = await fetch(releaseAsset('release-manifest.json', tag), { cache: 'no-store' })
  if (!response.ok) throw new Error(`${label} manifest could not be downloaded.`)
  const release = await response.json() as { firmware?: { factory?: Manifest } }
  const nextManifest = release.firmware?.factory
  if (!nextManifest || typeof nextManifest !== 'object' || typeof nextManifest.version !== 'string' ||
      nextManifest.protocol !== UPDATE_PROTOCOL || nextManifest.secureVersion !== 0 ||
      !Array.isArray(nextManifest.images) || nextManifest.images.length !== 1) {
    throw new Error(`${label} manifest is incomplete.`)
  }
  const image = nextManifest.images[0]
  if (!image || typeof image.name !== 'string' || typeof image.file !== 'string' ||
      !/^[A-Za-z0-9._-]+\.uf2$/.test(image.file) || image.address !== 0 ||
      !Number.isInteger(image.size) || image.size <= 0 || image.size > 4 * 1024 * 1024 ||
      typeof image.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(image.sha256)) {
    throw new Error(`${label} manifest contains an invalid flash image.`)
  }
  return { tag, manifest: nextManifest }
}

async function loadFirmware(tag: string | undefined, image: ManifestImage) {
  const response = await fetch(releaseAsset(image.file, tag), { cache: 'no-store' })
  if (!response.ok) throw new Error(`${image.name} could not be downloaded.`)
  const buffer = await response.arrayBuffer()
  if (buffer.byteLength !== image.size) throw new Error(`${image.name} has the wrong file size.`)
  if (await sha256(buffer) !== image.sha256) throw new Error(`${image.name} failed its integrity check.`)
  return URL.createObjectURL(new Blob([buffer], { type: 'application/octet-stream' }))
}

function download() {
  const current = manifest.value
  if (!current || !objectUrl.value) return
  const anchor = document.createElement('a')
  anchor.href = objectUrl.value
  anchor.download = current.images[0].file
  anchor.click()
  show('Copy the downloaded UF2 onto the RPI-RP2 drive. The board restarts when the drive disappears.', 'success')
}

async function selectTool() {
  const mode = selected.value
  manifest.value = null
  if (objectUrl.value) URL.revokeObjectURL(objectUrl.value)
  objectUrl.value = ''
  show('Loading firmware…')
  try {
    const loaded = await loadManifest(mode)
    const url = await loadFirmware(loaded.tag, loaded.manifest.images[0])
    if (selected.value !== mode) return
    manifest.value = loaded.manifest
    objectUrl.value = url
    show('')
  } catch (error) {
    if (selected.value !== mode) return
    const text = error instanceof Error ? error.message : String(error)
    show(/could not be downloaded/i.test(text)
      ? `${text} Check your internet connection, reload the page, and try again.`
      : text, 'error')
  }
}

onMounted(selectTool)
</script>

<template>
  <section class="flash-tool">
    <div class="flash-tool-controls">
      <label for="flash-version">Firmware</label>
      <select id="flash-version" v-model="selected" @change="selectTool">
        <option value="factory">Factory firmware</option>
        <option value="beta">Beta firmware</option>
      </select>
    </div>
    <div class="flash-tool-body">
      <p class="flash-description">Install tinyTouch on a Waveshare RP2040-Zero. The download is verified against the release manifest before it is offered.</p>
      <p class="flash-version">Version {{ manifest?.version ?? '…' }}</p>
      <button type="button" :disabled="!manifest || !objectUrl" @click="download">
        Download factory firmware (UF2)
      </button>
      <ol class="flash-steps">
        <li>Unplug tinyTouch.</li>
        <li>Hold <strong>BOOT</strong> while reconnecting it, or hold <strong>BOOT</strong> and tap <strong>RESET</strong>. A drive named <strong>RPI-RP2</strong> appears.</li>
        <li>Copy the UF2 onto <strong>RPI-RP2</strong>. The drive disappears when the board restarts.</li>
        <li>Unplug and reconnect once, then run <code>tinytouch setup</code>.</li>
      </ol>
      <p class="flash-description">To erase all stored keys and settings first, copy Raspberry Pi's <a href="https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#resetting-flash-memory">flash_nuke.uf2</a> onto RPI-RP2 before the tinyTouch UF2.</p>
      <div v-if="status" class="flash-status" :class="statusKind" role="status">{{ status }}</div>
    </div>
  </section>
</template>
