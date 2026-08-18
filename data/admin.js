const $ = (id) => document.getElementById(id);

let serviceUnlockEnabled = false;
let referencePage = 1;
let currentReferences = [];
let latestDevice = null;
let manualConnections = [];
let selectedManualPins = [];
let activeManualPairKey = "";
let hoveredManualPin = null;
let editingReferenceFile = "";
let editingReferenceName = "";
let currentPreviewFile = "";

const REFERENCE_PAGE_SIZE = 20;
const HISTORY_LIMIT = 20;
const DEVICE_HISTORY_KEY = "cableTesterDeviceHistory";
const OPERATOR_HISTORY_KEY = "cableTesterOperatorHistory";
const PIN_COUNT = 1024;
const MATRIX_SIDE = 32;
const ROW_BYTES = PIN_COUNT / 8;
const MATRIX_BYTES = PIN_COUNT * ROW_BYTES;
const REFERENCE_MAGIC = 0x31464243;
const FILE_FORMAT_VERSION = 3;
const PACKED_BLOB_MAGIC = 0x314b4350;
const REFERENCE_ANNOTATIONS_MAGIC = 0x314e4e41;
const REFERENCE_HEADER_BYTES = 756;

function toast(message) {
  const el = $("toast");
  if (!el) return;
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => {
    el.hidden = true;
  }, 4200);
}

async function api(url, options = {}) {
  const response = await fetch(url, { cache: "no-store", ...options });
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    data = text;
  }
  if (!response.ok) throw new Error(data?.error || text || `HTTP ${response.status}`);
  return data;
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function formatStoredDate(value) {
  if (!value) return "не указана";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString("ru-RU");
}

function fileSize(bytes) {
  if (bytes < 1024) return `${bytes} Б`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} КиБ`;
  return `${(bytes / 1024 / 1024).toFixed(1)} МиБ`;
}

function contactLabel(pinZeroBased) {
  return `Контакт ${Number(pinZeroBased) + 1}`;
}

function logicalPinText(value) {
  const text = String(value ?? "").trim();
  return text.slice(0, 31);
}

function parseContactValue(value) {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 1 || numeric > PIN_COUNT) return null;
  return numeric - 1;
}

function pairKey(a, b) {
  const left = Math.min(a, b);
  const right = Math.max(a, b);
  return `${left}:${right}`;
}

function normalizeShortText(value, maxLength) {
  return String(value ?? "").trim().slice(0, maxLength);
}

function stringHash(value) {
  let hash = 0;
  const text = String(value ?? "");
  for (let index = 0; index < text.length; index += 1) {
    hash = ((hash << 5) - hash + text.charCodeAt(index)) | 0;
  }
  return Math.abs(hash);
}

function harnessColor(mark) {
  const text = String(mark ?? "").trim();
  if (!text) {
    return {
      bg: "#dbeafe",
      border: "#60a5fa",
      pin: "#1d4ed8",
      text: "#1e3a8a",
      meta: "#334155"
    };
  }

  const hue = stringHash(text) % 360;
  return {
    bg: `hsl(${hue} 88% 86%)`,
    border: `hsl(${hue} 70% 58%)`,
    pin: `hsl(${hue} 82% 28%)`,
    text: `hsl(${hue} 58% 18%)`,
    meta: `hsl(${hue} 34% 30%)`
  };
}

function makeManualPair(a, b, metadata = {}) {
  const left = Math.min(Number(a), Number(b));
  const right = Math.max(Number(a), Number(b));
  const leftIsOriginalA = Number(a) === left;
  return {
    a: left,
    b: right,
    key: pairKey(left, right),
    wireName: normalizeShortText(metadata.wireName, 63),
    markA: normalizeShortText(leftIsOriginalA ? metadata.markA : metadata.markB, 31),
    localPinA: logicalPinText(leftIsOriginalA ? metadata.localPinA : metadata.localPinB),
    markB: normalizeShortText(leftIsOriginalA ? metadata.markB : metadata.markA, 31),
    localPinB: logicalPinText(leftIsOriginalA ? metadata.localPinB : metadata.localPinA),
    note: normalizeShortText(metadata.note, 127)
  };
}

function pairEndpointText(mark, logicalPin, physicalPin) {
  const markText = normalizeShortText(mark, 31);
  const logicalText = logicalPinText(logicalPin);
  if (markText && logicalText) return `${markText}/${logicalText}`;
  if (markText) return `${markText}/${Number(physicalPin) + 1}`;
  if (logicalText) return logicalText;
  return contactLabel(physicalPin);
}

function pairDisplayText(pair) {
  const left = pairEndpointText(pair.markA, pair.localPinA, pair.a);
  const right = pairEndpointText(pair.markB, pair.localPinB, pair.b);
  const wire = pair.wireName ? `Провод ${pair.wireName}` : "Связь";
  return `${wire}: ${left} ↔ ${right}`;
}

function pairMetadataText(pair) {
  const items = [];
  if (pair.wireName) items.push(`провод ${pair.wireName}`);
  if (pair.markA || pair.localPinA) items.push(`A=${pairEndpointText(pair.markA, pair.localPinA, pair.a)}`);
  if (pair.markB || pair.localPinB) items.push(`B=${pairEndpointText(pair.markB, pair.localPinB, pair.b)}`);
  items.push(`каналы=${Number(pair.a) + 1}↔${Number(pair.b) + 1}`);
  if (pair.note) items.push(`примечание: ${pair.note}`);
  return items.join(", ");
}

function selectedManualPair() {
  return manualConnections.find((pair) => pair.key === activeManualPairKey) || null;
}

function approvalStatusLabel(value) {
  if (value === "approved") return "Утверждён";
  if (value === "pending") return "На утверждении";
  if (value === "rejected") return "Отклонён";
  if (value === "archived") return "Архивный";
  return "Черновик";
}

function serviceUnlockMessage() {
  return "Сервисный тумблер выключен. Включите его для создания, загрузки и удаления.";
}

function readHistory(key) {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((item) => typeof item === "string" && item.trim()) : [];
  } catch {
    return [];
  }
}

function writeHistory(key, values) {
  localStorage.setItem(key, JSON.stringify(values.slice(0, HISTORY_LIMIT)));
}

function rememberHistoryValue(key, value) {
  const normalized = String(value ?? "").trim();
  if (!normalized) return;
  const unique = [normalized, ...readHistory(key).filter((item) => item !== normalized)];
  writeHistory(key, unique);
}

function renderSuggestionList(listId, values) {
  const list = $(listId);
  if (!list) return;
  list.innerHTML = "";
  for (const value of values) {
    const option = document.createElement("option");
    option.value = value;
    list.append(option);
  }
}

function refreshInputSuggestions() {
  renderSuggestionList("deviceSuggestions", readHistory(DEVICE_HISTORY_KEY));
  renderSuggestionList("operatorSuggestions", readHistory(OPERATOR_HISTORY_KEY));
}

function rememberCurrentMetadata() {
  rememberHistoryValue(DEVICE_HISTORY_KEY, $("deviceId")?.value);
  rememberHistoryValue(OPERATOR_HISTORY_KEY, $("operatorName")?.value);
  refreshInputSuggestions();
}

function primeSuggestionsFromReferences(records) {
  for (const item of Array.isArray(records) ? records : []) {
    rememberHistoryValue(DEVICE_HISTORY_KEY, item?.deviceId);
    rememberHistoryValue(OPERATOR_HISTORY_KEY, item?.operator);
  }
}

function browserTimeObject() {
  return {
    epochMs: Date.now(),
    offsetMinutes: -new Date().getTimezoneOffset(),
    timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC"
  };
}

function browserTimeParams() {
  const params = new URLSearchParams();
  const time = browserTimeObject();
  params.set("epochMs", String(time.epochMs));
  params.set("offsetMinutes", String(time.offsetMinutes));
  params.set("timeZone", time.timeZone);
  return params;
}

function parseMappingCrcValue() {
  const value = $("mappingCrc32")?.value.trim() || "";
  if (!value) return 0;
  const normalized = value.replace(/^0x/i, "");
  if (!/^[0-9a-f]{1,8}$/i.test(normalized)) throw new Error("CRC32 должен содержать до 8 hex-символов.");
  return Number.parseInt(normalized, 16) >>> 0;
}

function encodeFixedString(value, size) {
  const bytes = new Uint8Array(size);
  const encoded = new TextEncoder().encode(String(value ?? ""));
  bytes.set(encoded.slice(0, Math.max(0, size - 1)));
  return bytes;
}

function writeFixedString(view, offset, size, value) {
  const bytes = encodeFixedString(value, size);
  for (let i = 0; i < size; i += 1) view.setUint8(offset + i, bytes[i]);
}

function sanitizeReferenceFileBase(value) {
  let result = "";
  for (const char of String(value ?? "")) {
    if (result.length >= 60) break;
    if (/[A-Za-z0-9_-]/.test(char)) result += char;
    else if (char === " " || char === ".") result += "_";
  }
  if (!result) result = `reference_${Date.now()}`;
  return result;
}

function makeCrc32Table() {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    table[n] = c >>> 0;
  }
  return table;
}

const CRC32_TABLE = makeCrc32Table();

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = CRC32_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (~crc) >>> 0;
}

function packRle(bytes) {
  const output = [];
  let src = 0;
  while (src < bytes.length) {
    let runLength = 1;
    while (src + runLength < bytes.length && bytes[src + runLength] === bytes[src] && runLength < 128) runLength += 1;
    if (runLength >= 4) {
      output.push(0x80 | (runLength - 1), bytes[src]);
      src += runLength;
      continue;
    }

    const literalStart = src;
    let literalLength = 0;
    while (src < bytes.length && literalLength < 128) {
      runLength = 1;
      while (src + runLength < bytes.length && bytes[src + runLength] === bytes[src] && runLength < 128) runLength += 1;
      if (runLength >= 4 && literalLength > 0) break;
      src += runLength >= 4 ? 0 : 1;
      literalLength += 1;
      if (runLength >= 4) break;
    }
    output.push(literalLength - 1);
    for (let i = 0; i < literalLength; i += 1) output.push(bytes[literalStart + i]);
    src = literalStart + literalLength;
  }
  return new Uint8Array(output);
}

function buildManualMatrixBytes() {
  const matrix = new Uint8Array(MATRIX_BYTES);
  const setBit = (row, column) => {
    const offset = row * ROW_BYTES + (column >> 3);
    matrix[offset] |= (1 << (column & 7));
  };
  for (const pair of manualConnections) {
    setBit(pair.a, pair.b);
    setBit(pair.b, pair.a);
  }
  return matrix;
}

function buildPackedPayload(matrixBytes, matrixCrc) {
  const packed = packRle(matrixBytes);
  const payload = new Uint8Array(20 + packed.length);
  const view = new DataView(payload.buffer);
  view.setUint32(0, PACKED_BLOB_MAGIC, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, 0, true);
  view.setUint32(8, matrixBytes.length, true);
  view.setUint32(12, packed.length, true);
  view.setUint32(16, matrixCrc, true);
  payload.set(packed, 20);
  return payload;
}

function buildReferenceHeader(metadata, time, packedPayloadSize, matrixCrc) {
  const buffer = new ArrayBuffer(REFERENCE_HEADER_BYTES);
  const view = new DataView(buffer);
  view.setUint32(0, REFERENCE_MAGIC, true);
  view.setUint16(4, FILE_FORMAT_VERSION, true);
  view.setUint16(6, REFERENCE_HEADER_BYTES, true);
  view.setUint16(8, PIN_COUNT, true);
  view.setUint16(10, ROW_BYTES, true);
  view.setUint32(12, packedPayloadSize, true);
  view.setUint32(16, matrixCrc, true);
  view.setUint32(20, metadata.mappingCrc32 >>> 0, true);
  view.setBigUint64(24, BigInt(time.epochMs), true);
  view.setInt16(32, time.offsetMinutes, true);
  view.setUint16(34, 0, true);

  let offset = 36;
  const writeField = (size, value) => {
    writeFixedString(view, offset, size, value);
    offset += size;
  };

  writeField(64, metadata.name);
  writeField(64, metadata.cableType);
  writeField(32, metadata.revision);
  writeField(64, metadata.deviceId);
  writeField(32, latestDevice?.deviceModel || "KSK-1024");
  writeField(24, latestDevice?.firmwareVersion || "2.1.0");
  writeField(96, metadata.operatorName);
  writeField(64, time.timeZone);
  writeField(256, metadata.comment);
  writeField(24, metadata.approvalStatus);

  return new Uint8Array(buffer);
}

function buildAnnotationsPayload() {
  const annotations = {
    version: 1,
    connections: manualConnections.map((pair) => ({
      a: pair.a,
      b: pair.b,
      wireName: pair.wireName || "",
      markA: pair.markA || "",
      localPinA: pair.localPinA || "",
      markB: pair.markB || "",
      localPinB: pair.localPinB || "",
      note: pair.note || ""
    }))
  };
  const jsonBytes = new TextEncoder().encode(JSON.stringify(annotations.connections));
  const payload = new Uint8Array(16 + jsonBytes.length);
  const view = new DataView(payload.buffer);
  view.setUint32(0, REFERENCE_ANNOTATIONS_MAGIC, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, 0, true);
  view.setUint32(8, jsonBytes.length, true);
  view.setUint32(12, crc32(jsonBytes), true);
  payload.set(jsonBytes, 16);
  return payload;
}

async function uploadGeneratedReference(fileName, fileBytes) {
  const form = new FormData();
  const blob = new Blob([fileBytes], { type: "application/octet-stream" });
  form.append("file", blob, fileName);
  const response = await fetch("/api/upload/reference", { method: "POST", body: form, cache: "no-store" });
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    data = text;
  }
  if (!response.ok) throw new Error(data?.error || text || `HTTP ${response.status}`);
  return data;
}

function collectReferenceMetadata() {
  const required = [
    ["referenceName", "Введите название эталона."],
    ["cableType", "Введите тип кабельной сборки."],
    ["revision", "Введите ревизию."],
    ["deviceId", "Укажите прибор."],
    ["operatorName", "Укажите оператора."]
  ];
  for (const [id, message] of required) {
    if (!$(id)?.value.trim()) throw new Error(message);
  }

  return {
    name: $("referenceName").value.trim(),
    cableType: $("cableType").value.trim(),
    revision: $("revision").value.trim(),
    deviceId: $("deviceId").value.trim(),
    operatorName: $("operatorName").value.trim(),
    comment: $("referenceComment")?.value.trim() || "",
    approvalStatus: $("approvalStatus")?.value || "draft",
    mappingCrc32: parseMappingCrcValue()
  };
}

function referenceMetadataParams() {
  const metadata = collectReferenceMetadata();
  const params = browserTimeParams();
  params.set("name", metadata.name);
  params.set("cableType", metadata.cableType);
  params.set("revision", metadata.revision);
  params.set("deviceId", metadata.deviceId);
  params.set("operator", metadata.operatorName);
  params.set("mappingCrc32", metadata.mappingCrc32 ? metadata.mappingCrc32.toString(16).toUpperCase() : "");
  params.set("approvalStatus", metadata.approvalStatus);
  params.set("comment", metadata.comment);
  return params;
}

function fillMetadataForm(item) {
  if (!item) return;
  if ($("referenceName")) $("referenceName").value = item.name || "";
  if ($("cableType")) $("cableType").value = item.cableType || "";
  if ($("revision")) $("revision").value = item.revision || "";
  if ($("deviceId")) $("deviceId").value = item.deviceId || "";
  if ($("operatorName")) $("operatorName").value = item.operator || "";
  if ($("mappingCrc32")) $("mappingCrc32").value = item.mappingCrc32 ? Number(item.mappingCrc32).toString(16).toUpperCase() : "";
  if ($("approvalStatus")) $("approvalStatus").value = item.approvalStatus || "draft";
  if ($("referenceComment")) $("referenceComment").value = item.comment || "";
  rememberCurrentMetadata();
}

function fillManualPairInputs(pair) {
  if ($("manualContactA")) $("manualContactA").value = pair ? String(pair.a + 1) : "";
  if ($("manualContactB")) $("manualContactB").value = pair ? String(pair.b + 1) : "";
  if ($("manualWireName")) $("manualWireName").value = pair?.wireName || "";
  if ($("manualMarkA")) $("manualMarkA").value = pair?.markA || "";
  if ($("manualLocalPinA")) $("manualLocalPinA").value = pair?.localPinA || "";
  if ($("manualMarkB")) $("manualMarkB").value = pair?.markB || "";
  if ($("manualLocalPinB")) $("manualLocalPinB").value = pair?.localPinB || "";
  if ($("manualNote")) $("manualNote").value = pair?.note || "";
}

function renderManualStatus() {
  const status = $("manualEditorStatus");
  if (!status) return;
  if (!manualConnections.length) {
    status.textContent = selectedManualPins.length === 1
      ? `Выбран ${contactLabel(selectedManualPins[0])}. Выберите второй контакт для добавления связи.`
      : "Пока связей нет. Выберите два контакта, чтобы добавить первую пару.";
    return;
  }
  const activePair = selectedManualPair();
  status.textContent = activePair
    ? `Добавлено связей: ${manualConnections.length}. Открыта пара: ${pairDisplayText(activePair)}.`
    : `Добавлено связей: ${manualConnections.length}.`;
}

function renderEditingState() {
  const target = $("manualEditingState");
  if (!target) return;
  target.textContent = editingReferenceFile
    ? `Открыт эталон для редактирования: ${editingReferenceName || editingReferenceFile}. При сохранении можно перезаписать его или создать новый файл.`
    : "Сейчас открыт режим создания нового ручного эталона.";
}

function renderManualPairList() {
  const target = $("manualPairList");
  if (!target) return;
  if (!manualConnections.length) {
    target.className = "empty";
    target.textContent = "Список связей пока пуст.";
    return;
  }

  target.className = "manual-pair-list";
  target.innerHTML = manualConnections.map((pair, index) => `
    <div class="manual-pair-item ${pair.key === activeManualPairKey ? "active" : ""}" data-manual-pair="${pair.key}">
      <div>
        <strong>Связь ${index + 1}</strong><br>
        ${escapeHtml(pairDisplayText(pair))}
        <div class="manual-pair-meta">${escapeHtml(pairMetadataText(pair) || "без дополнительной маркировки")}</div>
      </div>
      <button type="button" data-delete-manual-pair="${pair.key}">Удалить</button>
    </div>
  `).join("");

  target.querySelectorAll("[data-manual-pair]").forEach((item) => {
    item.addEventListener("click", (event) => {
      if (event.target instanceof HTMLElement && event.target.closest("[data-delete-manual-pair]")) return;
      setActiveManualPair(item.dataset.manualPair || "");
    });
  });

  target.querySelectorAll("[data-delete-manual-pair]").forEach((button) => {
    button.addEventListener("click", (event) => {
      event.stopPropagation();
      manualConnections = manualConnections.filter((pair) => pair.key !== button.dataset.deleteManualPair);
      if (activeManualPairKey === button.dataset.deleteManualPair) activeManualPairKey = "";
      renderManualEditor();
    });
  });
}

function buildManualGuideAssignments() {
  const assignments = new Map();

  const addEndpoint = (pin, mark, localPin, wireName, note) => {
    const physicalPin = Number(pin);
    if (!Number.isInteger(physicalPin) || physicalPin < 0 || physicalPin >= PIN_COUNT) return;
    const normalizedMark = String(mark ?? "").trim();
    const endpoint = pairEndpointText(mark, localPin, physicalPin);
    const metaParts = [];
    if (wireName) metaParts.push(`провод ${wireName}`);
    if (note) metaParts.push(note);
    const meta = metaParts.join(" · ");
    if (!assignments.has(physicalPin)) {
      assignments.set(physicalPin, {
        pin: physicalPin,
        mark: normalizedMark,
        endpoint,
        metas: meta ? new Set([meta]) : new Set()
      });
      return;
    }
    const existing = assignments.get(physicalPin);
    if (!existing.mark && normalizedMark) existing.mark = normalizedMark;
    if (existing.endpoint !== endpoint) existing.endpoint = `${existing.endpoint} / ${endpoint}`;
    if (meta) existing.metas.add(meta);
  };

  for (const pair of manualConnections) {
    addEndpoint(pair.a, pair.markA, pair.localPinA, pair.wireName, pair.note);
    addEndpoint(pair.b, pair.markB, pair.localPinB, pair.wireName, pair.note);
  }

  return Array.from(assignments.values()).sort((left, right) => left.pin - right.pin);
}

function renderManualGuideLegend(assignments) {
  const legend = $("manualGuideLegend");
  if (!legend) return;
  const marks = Array.from(new Set(assignments.map((item) => String(item?.mark || "").trim()).filter(Boolean)))
    .sort((left, right) => left.localeCompare(right, "ru"));
  if (!marks.length) {
    legend.innerHTML = '<div class="empty">Легенда жгутов появится после добавления связей.</div>';
    return;
  }
  legend.innerHTML = marks.map((mark) => {
    const colors = harnessColor(mark);
    return `
      <div class="reference-guide-legend-item">
        <span class="reference-guide-legend-swatch" style="background:${escapeHtml(colors.bg)};border-color:${escapeHtml(colors.border)};"></span>
        <span>${escapeHtml(mark)}</span>
      </div>
    `;
  }).join("");
}

function handleManualPinSelection(pin) {
  if (selectedManualPins.length && selectedManualPins[0] === pin) {
    selectedManualPins = [];
    renderManualEditor();
    return;
  }
  selectedManualPins = [...selectedManualPins.filter((value) => value !== pin), pin].slice(-2);
  if (selectedManualPins.length === 2) {
    const [a, b] = selectedManualPins;
    if (a !== b) {
      addOrReplaceManualPair(a, b, {
        wireName: $("manualWireName")?.value,
        markA: $("manualMarkA")?.value,
        localPinA: $("manualLocalPinA")?.value,
        markB: $("manualMarkB")?.value,
        localPinB: $("manualLocalPinB")?.value,
        note: $("manualNote")?.value
      });
    }
    selectedManualPins = [];
  }
  renderManualEditor();
}

function renderManualGuideGrid() {
  const grid = $("manualGuideGrid");
  if (!grid) return;

  const assignments = buildManualGuideAssignments();
  renderManualGuideLegend(assignments);
  const activePair = selectedManualPair();
  const activePins = activePair ? [activePair.a, activePair.b] : [];

  const blocks = Array.from({ length: 16 }, (_, index) => ({ index, items: [] }));
  for (const item of assignments) {
    const blockIndex = Math.floor(item.pin / 64);
    const positionInBlock = (item.pin % 64) + 1;
    blocks[blockIndex].items.push({ ...item, positionInBlock });
  }

  grid.innerHTML = blocks.map((block) => {
    const start = block.index * 64 + 1;
    const end = start + 63;
    const itemMap = new Map(block.items.map((item) => [item.positionInBlock, item]));
    const rows = Array.from({ length: 64 }, (_, positionIndex) => {
      const position = positionIndex + 1;
      const pin = block.index * 64 + positionIndex;
      const item = itemMap.get(position);
      const colors = harnessColor(item?.mark || "");
      const selected = selectedManualPins.includes(pin);
      const active = activePins.includes(pin);
      return `
        <div class="reference-guide-cell ${item ? "occupied" : ""} ${selected ? "selected" : ""} ${active ? "focused" : ""}" data-manual-pin="${pin}" style="${item ? `--guide-bg:${colors.bg};--guide-border:${colors.border};--guide-pin:${colors.pin};--guide-text:${colors.text};--guide-meta:${colors.meta};` : ""}" title="${escapeHtml(item ? `${item.endpoint}${item.metas.size ? ` | ${Array.from(item.metas).join(" | ")}` : ""}` : `Позиция ${position} не используется`)}">
          <div class="reference-guide-cell-pin">${escapeHtml(String(position))}</div>
          ${item ? `
            <div class="reference-guide-cell-body">
              <div class="reference-guide-cell-endpoint">${escapeHtml(item.endpoint)}</div>
              <div class="reference-guide-cell-meta">${escapeHtml(String(item.pin + 1))}</div>
            </div>
          ` : '<div class="reference-guide-cell-empty">—</div>'}
        </div>
      `;
    }).join("");
    return `
      <div class="reference-guide-card">
        <div class="reference-guide-head">
          <strong>Блок ${escapeHtml(String(block.index + 1))}</strong>
          <span>Каналы ${escapeHtml(String(start))}–${escapeHtml(String(end))}</span>
        </div>
        <div class="reference-guide-body">
          <div class="reference-guide-panel">${rows}</div>
        </div>
      </div>
    `;
  }).join("");

  grid.querySelectorAll("[data-manual-pin]").forEach((cell) => {
    cell.addEventListener("click", () => handleManualPinSelection(Number(cell.dataset.manualPin)));
    cell.addEventListener("mouseenter", () => updateManualHoverInfo(Number(cell.dataset.manualPin)));
    cell.addEventListener("mouseleave", () => updateManualHoverInfo(null));
  });
}

function updateManualHoverInfo(pin) {
  hoveredManualPin = pin;
  const target = $("manualHoverInfo");
  if (!target) return;
  if (pin === null || pin === undefined) {
    target.textContent = "Выберите два контакта в блоках, чтобы добавить связь.";
    return;
  }
  const pairCount = manualConnections.filter((pair) => pair.a === pin || pair.b === pin).length;
  target.textContent = `${contactLabel(pin)}. Связей с этим контактом: ${pairCount}.`;
}

function renderManualEditor() {
  renderEditingState();
  renderManualStatus();
  renderManualPairList();
  renderManualGuideGrid();
  fillManualPairInputs(selectedManualPair());
}

function persistSelectedManualPairEdits() {
  const pair = selectedManualPair();
  if (!pair) return;
  pair.wireName = normalizeShortText($("manualWireName")?.value, 63);
  pair.markA = normalizeShortText($("manualMarkA")?.value, 31);
  pair.localPinA = logicalPinText($("manualLocalPinA")?.value);
  pair.markB = normalizeShortText($("manualMarkB")?.value, 31);
  pair.localPinB = logicalPinText($("manualLocalPinB")?.value);
  pair.note = normalizeShortText($("manualNote")?.value, 127);
  renderManualPairList();
  renderManualStatus();
}

function setActiveManualPair(key) {
  activeManualPairKey = key || "";
  renderManualEditor();
}

function addOrReplaceManualPair(a, b, metadata = {}) {
  const normalized = makeManualPair(a, b, metadata);
  const index = manualConnections.findIndex((pair) => pair.key === normalized.key);
  if (index >= 0) manualConnections[index] = normalized;
  else manualConnections.push(normalized);
  manualConnections.sort((left, right) => (left.a - right.a) || (left.b - right.b));
  activeManualPairKey = normalized.key;
}

function addManualPairFromInputs() {
  const a = parseContactValue($("manualContactA")?.value);
  const b = parseContactValue($("manualContactB")?.value);
  if (a === null || b === null) return toast("Укажите оба контакта от 1 до 1024.");
  if (a === b) return toast("Нельзя соединить контакт сам с собой.");
  addOrReplaceManualPair(a, b, {
    wireName: $("manualWireName")?.value,
    markA: $("manualMarkA")?.value,
    localPinA: $("manualLocalPinA")?.value,
    markB: $("manualMarkB")?.value,
    localPinB: $("manualLocalPinB")?.value,
    note: $("manualNote")?.value
  });
  selectedManualPins = [];
  renderManualEditor();
}

function parseDelimitedRow(line, delimiter) {
  const cells = [];
  let current = "";
  let quoted = false;
  for (let i = 0; i < line.length; i += 1) {
    const char = line[i];
    if (char === '"') {
      if (quoted && line[i + 1] === '"') {
        current += '"';
        i += 1;
      } else {
        quoted = !quoted;
      }
      continue;
    }
    if (!quoted && char === delimiter) {
      cells.push(current.trim());
      current = "";
      continue;
    }
    current += char;
  }
  cells.push(current.trim());
  return cells;
}

function detectDelimiter(text) {
  const firstLine = text.split(/\r?\n/).find((line) => line.trim()) || "";
  const tabCount = (firstLine.match(/\t/g) || []).length;
  const semicolonCount = (firstLine.match(/;/g) || []).length;
  const commaCount = (firstLine.match(/,/g) || []).length;
  if (tabCount >= semicolonCount && tabCount >= commaCount && tabCount > 0) return "\t";
  if (semicolonCount >= commaCount && semicolonCount > 0) return ";";
  return ",";
}

function csvHeaderLike(value) {
  return ["провод", "позиция", "маркировка", "контакт", "примечание"].includes(String(value || "").trim().toLowerCase());
}

function allocateVirtualContact(endpointKey, endpointMap) {
  if (endpointMap.has(endpointKey)) return endpointMap.get(endpointKey);
  const next = endpointMap.size;
  if (next >= PIN_COUNT) {
    throw new Error(`В CSV больше ${PIN_COUNT} уникальных логических точек. Сократите файл или разбейте его.`);
  }
  endpointMap.set(endpointKey, next);
  return next;
}

function parseHarnessLogicalRow(row, rowIndex, rowText, endpointMap) {
  const wireName = String(row[0] || "").trim();
  const position = String(row[1] || "").trim();
  const markA = String(row[2] || "").trim();
  const localPinA = String(row[3] || "").trim();
  const markB = String(row[4] || "").trim();
  const localPinB = String(row[5] || "").trim();
  const note = String(row[6] || "").trim();

  if (!wireName && !position && !markA && !localPinA && !markB && !localPinB && !note) return null;
  if (!markA || !localPinA || !markB || !localPinB) {
    throw new Error(`Ошибка в строке ${rowIndex}: должны быть заполнены поля Маркировка/Контакт для обоих концов. Ожидаемый формат: Провод, Позиция, Маркировка, Контакт, Маркировка, Контакт, Примечание. Строка: ${rowText}`);
  }

  const endpointA = `${markA}::${localPinA}`;
  const endpointB = `${markB}::${localPinB}`;
  if (endpointA === endpointB) {
    throw new Error(`Ошибка в строке ${rowIndex}: логическая точка не может быть соединена сама с собой. Строка: ${rowText}`);
  }

  const contactA = allocateVirtualContact(endpointA, endpointMap);
  const contactB = allocateVirtualContact(endpointB, endpointMap);
  const combinedNote = [position ? `позиция: ${position}` : "", note].filter(Boolean).join("; ");
  return makeManualPair(contactA, contactB, { wireName, markA, localPinA, markB, localPinB, note: combinedNote });
}

function parseManualCsv(text) {
  const delimiter = detectDelimiter(text);
  const rows = text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  if (!rows.length) throw new Error("CSV пустой.");

  const parsed = rows.map((line) => parseDelimitedRow(line, delimiter));
  const dataRows = [];
  const endpointMap = new Map();
  for (let i = 0; i < parsed.length; i += 1) {
    const row = parsed[i];
    const rowText = row.join(" | ");
    const first = String(row[0] || "").trim().toLowerCase();
    if (!first || csvHeaderLike(first)) continue;

    const pair = parseHarnessLogicalRow(row, i + 1, rowText, endpointMap);
    if (pair) dataRows.push(pair);
  }
  return dataRows;
}

async function importManualCsv() {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  const file = $("manualCsvInput")?.files?.[0];
  if (!file) return toast("Сначала выберите CSV файл.");
  try {
    const text = await file.text();
    const pairs = parseManualCsv(text);
    if (!pairs.length) throw new Error("В CSV не найдено ни одной связи.");
    if (manualConnections.length && !confirm("Заменить текущий список связей данными из CSV?")) return;
    manualConnections = pairs;
    activeManualPairKey = pairs[0]?.key || "";
    selectedManualPins = [];
    renderManualEditor();
    toast(`Импортировано связей: ${pairs.length}.`);
  } catch (error) {
    toast(error.message);
  }
}

function sortReferenceReports(files) {
  const sortMode = $("referenceSort")?.value || "date_desc";
  const reports = [...files];
  reports.sort((left, right) => {
    const leftDate = Date.parse(left?.createdAtLocal || "") || 0;
    const rightDate = Date.parse(right?.createdAtLocal || "") || 0;
    if (sortMode === "date_asc") return leftDate - rightDate;
    if (sortMode === "name_asc") return String(left.name || left.file).localeCompare(String(right.name || right.file), "ru");
    if (sortMode === "name_desc") return String(right.name || right.file).localeCompare(String(left.name || left.file), "ru");
    return rightDate - leftDate;
  });
  return reports;
}

function renderReferencePreview(item, view = null) {
  const target = $("referencePreview");
  if (!target) return;
  if (!item && !view) {
    target.innerHTML = '<div class="message">Выберите эталон в журнале, чтобы посмотреть его данные.</div>';
    return;
  }

  const source = item || {};
  const annotations = Array.isArray(view?.annotations) ? view.annotations : [];
  const previewList = annotations.slice(0, 24).map((pair, index) => `
    <div class="manual-pair-item">
      <div>
        <strong>Связь ${index + 1}</strong><br>
        ${escapeHtml(pairDisplayText({
          a: Number(pair.a),
          b: Number(pair.b),
          wireName: pair.wireName || "",
          markA: pair.markA || "",
          localPinA: pair.localPinA || "",
          markB: pair.markB || "",
          localPinB: pair.localPinB || "",
          note: pair.note || ""
        }))}
      </div>
    </div>
  `).join("");

  target.innerHTML = `
    <div class="admin-preview-grid">
      <div class="admin-preview-item"><strong>Название</strong><span>${escapeHtml(source.name || view?.title || source.file || "")}</span></div>
      <div class="admin-preview-item"><strong>Дата</strong><span>${escapeHtml(formatStoredDate(source.createdAtLocal || ""))}</span></div>
      <div class="admin-preview-item"><strong>Тип сборки</strong><span>${escapeHtml(source.cableType || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Ревизия</strong><span>${escapeHtml(source.revision || "не указана")}</span></div>
      <div class="admin-preview-item"><strong>Прибор</strong><span>${escapeHtml(source.deviceId || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Оператор</strong><span>${escapeHtml(source.operator || "не указан")}</span></div>
      <div class="admin-preview-item"><strong>Статус</strong><span>${escapeHtml(approvalStatusLabel(source.approvalStatus))}</span></div>
      <div class="admin-preview-item"><strong>Размер</strong><span>${escapeHtml(fileSize(source.size || 0))}</span></div>
      <div class="admin-preview-item"><strong>Связей</strong><span>${escapeHtml(String(view?.connectionCount ?? annotations.length ?? 0))}</span></div>
      <div class="admin-preview-item"><strong>С маркировкой</strong><span>${escapeHtml(String(annotations.filter((pair) => pair.wireName || pair.markA || pair.markB).length))}</span></div>
    </div>
    <div class="manual-pair-list-wrap">
      ${previewList || '<div class="message">Дополнительных сведений по проводам пока нет.</div>'}
    </div>
  `;
}

async function openReferenceForEditing(fileName) {
  try {
    const [item, view] = await Promise.all([
      Promise.resolve(currentReferences.find((entry) => entry.file === fileName) || null),
      api(`/api/reference/view?file=${encodeURIComponent(fileName)}`)
    ]);

    if (item) {
      fillMetadataForm(item);
      editingReferenceName = item.name || fileName;
    } else {
      editingReferenceName = fileName;
    }
    editingReferenceFile = fileName;
    currentPreviewFile = fileName;

    const annotations = Array.isArray(view?.annotations) && view.annotations.length
      ? view.annotations
      : (Array.isArray(view?.connections) ? view.connections : []);
    manualConnections = annotations
      .map((pair) => makeManualPair(Number(pair.a), Number(pair.b), {
        wireName: pair.wireName || "",
        markA: pair.markA || "",
        localPinA: pair.localPinA || "",
        markB: pair.markB || "",
        localPinB: pair.localPinB || "",
        note: pair.note || ""
      }))
      .filter((pair) => Number.isInteger(pair.a) && Number.isInteger(pair.b) && pair.a >= 0 && pair.b >= 0 && pair.a < PIN_COUNT && pair.b < PIN_COUNT);
    manualConnections.sort((left, right) => (left.a - right.a) || (left.b - right.b));
    selectedManualPins = [];
    activeManualPairKey = manualConnections[0]?.key || "";
    renderManualEditor();
    renderReferencePreview(item, view);
    $("saveManualReferenceButton")?.scrollIntoView({ behavior: "smooth", block: "center" });
    toast(`Эталон открыт для редактирования: ${editingReferenceName}`);
  } catch (error) {
    toast(error.message);
  }
}

async function saveManualReference() {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!manualConnections.length) return toast("Добавьте хотя бы одну связь в ручной эталон.");
  try {
    persistSelectedManualPairEdits();
    const metadata = collectReferenceMetadata();
    const time = browserTimeObject();
    rememberCurrentMetadata();

    const matrixBytes = buildManualMatrixBytes();
    const matrixCrc = crc32(matrixBytes);
    const packedPayload = buildPackedPayload(matrixBytes, matrixCrc);
    const annotationsPayload = buildAnnotationsPayload();
    const header = buildReferenceHeader(metadata, time, packedPayload.length, matrixCrc);
    const fileBytes = new Uint8Array(header.length + packedPayload.length + annotationsPayload.length);
    fileBytes.set(header, 0);
    fileBytes.set(packedPayload, header.length);
    fileBytes.set(annotationsPayload, header.length + packedPayload.length);

    let fileName = `${sanitizeReferenceFileBase(metadata.name)}.ref`;
    if (editingReferenceFile) {
      const overwrite = confirm(
        `Перезаписать открытый эталон ${editingReferenceName || editingReferenceFile}?\n\nНажмите OK для перезаписи.\nНажмите Отмена, чтобы сохранить как новый файл.`
      );
      if (overwrite) fileName = editingReferenceFile;
    }

    const response = await uploadGeneratedReference(fileName, fileBytes);
    editingReferenceFile = response.file || fileName;
    editingReferenceName = metadata.name;
    currentPreviewFile = editingReferenceFile;
    renderManualEditor();
    toast(`Ручной эталон сохранён: ${response.file || fileName}`);
    await loadReferences();
    await openReferenceForEditing(editingReferenceFile);
  } catch (error) {
    toast(error.message);
  }
}

function setAdminAccess(enabled) {
  serviceUnlockEnabled = Boolean(enabled);
  const note = $("serviceLockNote");
  const badge = $("serviceBadge");
  const text = $("serviceText");

  if (badge) {
    badge.textContent = serviceUnlockEnabled ? "Сервисный режим включён" : "Сервисный режим выключен";
    badge.className = `badge ${serviceUnlockEnabled ? "ok" : "error"}`;
  }
  if (text) {
    text.textContent = serviceUnlockEnabled
      ? "Создание эталонов, загрузка файлов и удаление записей доступны."
      : "Создание, загрузка и удаление заблокированы до включения сервисного тумблера.";
  }
  if (note) note.hidden = serviceUnlockEnabled;

  ["captureButton", "saveManualReferenceButton", "referenceUploadButton", "calculationUploadButton", "manualCsvImportButton"].forEach((id) => {
    if ($(id)) $(id).hidden = !serviceUnlockEnabled;
  });
  ["referenceUploadInput", "calculationUploadInput", "manualCsvInput"].forEach((id) => {
    if ($(id)) $(id).hidden = !serviceUnlockEnabled;
  });
  renderReferences(currentReferences);
}

async function loadDevice() {
  try {
    const device = await api("/api/device");
    latestDevice = device;
    if ($("deviceBadge")) {
      $("deviceBadge").textContent = `${device.deviceModel || device.hardware} · ${device.ip} · ${device.link ? "LINK" : "NO LINK"} · FW ${device.firmwareVersion || "?"}`;
      $("deviceBadge").className = `badge ${device.link ? "ok" : "error"}`;
    }
    if ($("engineBadge")) {
      const kind = device.engineBusy ? "busy" : device.engineState === "failed" ? "error" : device.engineState === "idle" ? "ok" : "neutral";
      $("engineBadge").className = `badge ${kind}`;
      $("engineBadge").textContent = device.engineBusy ? "Идёт операция" : (device.engineState || "idle");
    }
    if ($("engineText")) $("engineText").textContent = device.engineMessage || "Система готова.";
    setAdminAccess(device.serviceUnlockEnabled);
  } catch (error) {
    latestDevice = null;
    if ($("deviceBadge")) {
      $("deviceBadge").textContent = "Нет связи";
      $("deviceBadge").className = "badge error";
    }
    if ($("engineBadge")) {
      $("engineBadge").textContent = "Нет связи";
      $("engineBadge").className = "badge error";
    }
    if ($("engineText")) $("engineText").textContent = "Не удалось получить состояние устройства.";
    setAdminAccess(false);
  }
}

async function deleteReferenceFile(fileName) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  if (!confirm(`Удалить эталон ${fileName}?`)) return;
  try {
    await api(`/api/reference?file=${encodeURIComponent(fileName)}`, { method: "DELETE" });
    if (editingReferenceFile === fileName) {
      editingReferenceFile = "";
      editingReferenceName = "";
      currentPreviewFile = "";
      manualConnections = [];
      activeManualPairKey = "";
      selectedManualPins = [];
      renderManualEditor();
      renderReferencePreview(null, null);
    }
    toast("Эталон удалён.");
    await loadReferences();
  } catch (error) {
    toast(error.message);
  }
}

function renderReferences(files) {
  const target = $("referenceTable");
  const summary = $("referenceSummary");
  const pagination = $("referencePagination");
  if (!target || !summary || !pagination) return;

  const reports = sortReferenceReports((Array.isArray(files) ? files : []).filter((item) => item?.file?.endsWith(".ref")));
  summary.textContent = `${reports.length} записей`;

  if (!reports.length) {
    target.className = "empty";
    target.textContent = "Записей пока нет.";
    pagination.hidden = true;
    pagination.innerHTML = "";
    return;
  }

  const totalPages = Math.max(1, Math.ceil(reports.length / REFERENCE_PAGE_SIZE));
  referencePage = Math.min(Math.max(referencePage, 1), totalPages);
  const start = (referencePage - 1) * REFERENCE_PAGE_SIZE;
  const pageReports = reports.slice(start, start + REFERENCE_PAGE_SIZE);
  const rows = pageReports.map((item) => `
    <tr ${item.file === currentPreviewFile ? 'class="is-selected"' : ""}>
      <td>${escapeHtml(formatStoredDate(item.createdAtLocal || ""))}</td>
      <td class="wrap">${escapeHtml(item.name || item.file)}</td>
      <td class="wrap">${escapeHtml(item.cableType || "не указан")}</td>
      <td>${escapeHtml(item.revision || "не указана")}</td>
      <td>${escapeHtml(item.deviceId || "не указан")}</td>
      <td class="wrap">${escapeHtml(item.operator || "не указан")}</td>
      <td>${escapeHtml(approvalStatusLabel(item.approvalStatus))}</td>
      <td>
        <div class="journal-actions">
          <button type="button" data-open-reference="${escapeHtml(item.file)}">Открыть</button>
          <button type="button" data-delete-reference="${escapeHtml(item.file)}" ${serviceUnlockEnabled ? "" : "hidden"}>Удалить</button>
          <a href="/api/download?type=reference&file=${encodeURIComponent(item.file)}">Выгрузить</a>
        </div>
      </td>
    </tr>
  `).join("");

  target.className = "";
  target.innerHTML = `<div class="table-wrap"><table class="journal-table"><thead><tr><th>Дата</th><th class="wrap">Название</th><th class="wrap">Сборка</th><th>Ревизия</th><th>Прибор</th><th class="wrap">Оператор</th><th>Статус</th><th>Действие</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  pagination.hidden = false;
  pagination.innerHTML = `
    <button type="button" data-reference-page="prev" ${referencePage <= 1 ? "disabled" : ""}>Назад</button>
    <span class="journal-pagination-info">Страница ${referencePage} из ${totalPages}</span>
    <button type="button" data-reference-page="next" ${referencePage >= totalPages ? "disabled" : ""}>Вперёд</button>
  `;

  target.querySelectorAll("[data-open-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      await openReferenceForEditing(button.dataset.openReference);
    });
  });
  target.querySelectorAll("[data-delete-reference]").forEach((button) => {
    button.addEventListener("click", async () => {
      await deleteReferenceFile(button.dataset.deleteReference);
    });
  });
  pagination.querySelectorAll("[data-reference-page]").forEach((button) => {
    button.addEventListener("click", () => {
      referencePage += button.dataset.referencePage === "next" ? 1 : -1;
      renderReferences(files);
    });
  });
}

async function loadReferences() {
  try {
    currentReferences = await api("/api/references");
    primeSuggestionsFromReferences(currentReferences);
    refreshInputSuggestions();
    renderReferences(currentReferences);
    if (currentPreviewFile) {
      const item = currentReferences.find((entry) => entry.file === currentPreviewFile) || null;
      renderReferencePreview(item, null);
    }
  } catch (error) {
    toast(error.message);
  }
}

function uploadFile(kind, input, bar, text) {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  const file = input.files?.[0];
  if (!file) return toast("Сначала выберите файл.");

  const form = new FormData();
  form.append("file", file, file.name);
  if (text) text.textContent = `Загрузка: ${file.name}`;
  if (bar) bar.style.width = "20%";

  fetch(kind === "reference" ? "/api/upload/reference" : "/api/upload/calculation", {
    method: "POST",
    body: form,
    cache: "no-store"
  })
    .then(async (response) => {
      const payload = await response.text();
      let data;
      try {
        data = JSON.parse(payload);
      } catch {
        data = payload;
      }
      if (!response.ok) throw new Error(data?.error || payload || `HTTP ${response.status}`);
      if (bar) bar.style.width = "100%";
      if (text) text.textContent = `Загружено: ${data.file || file.name}`;
      toast(kind === "reference" ? "Эталон загружен." : "Отчёт загружен.");
      if (kind === "reference") loadReferences();
    })
    .catch((error) => {
      if (bar) bar.style.width = "0%";
      if (text) text.textContent = error.message;
      toast(error.message);
    });
}

async function startReferenceCapture() {
  if (!serviceUnlockEnabled) return toast(serviceUnlockMessage());
  try {
    rememberCurrentMetadata();
    await api(`/api/reference/capture?${referenceMetadataParams().toString()}`, { method: "POST" });
    toast("Эталонный замер запущен.");
  } catch (error) {
    toast(error.message);
  }
}

function resetManualEditor() {
  manualConnections = [];
  selectedManualPins = [];
  activeManualPairKey = "";
  editingReferenceFile = "";
  editingReferenceName = "";
  fillManualPairInputs(null);
  renderManualEditor();
}

$("captureButton")?.addEventListener("click", startReferenceCapture);
$("saveManualReferenceButton")?.addEventListener("click", saveManualReference);
$("addManualPairButton")?.addEventListener("click", addManualPairFromInputs);
$("clearManualPairsButton")?.addEventListener("click", () => {
  if (!manualConnections.length) return;
  if (!confirm("Очистить все ручные связи?")) return;
  resetManualEditor();
});
$("manualCsvImportButton")?.addEventListener("click", importManualCsv);
$("manualWireName")?.addEventListener("input", persistSelectedManualPairEdits);
$("manualMarkA")?.addEventListener("input", persistSelectedManualPairEdits);
$("manualMarkB")?.addEventListener("input", persistSelectedManualPairEdits);
$("manualNote")?.addEventListener("input", persistSelectedManualPairEdits);
$("referenceUploadButton")?.addEventListener("click", () => uploadFile("reference", $("referenceUploadInput"), $("referenceUploadBar"), $("referenceUploadText")));
$("calculationUploadButton")?.addEventListener("click", () => uploadFile("calculation", $("calculationUploadInput"), $("calculationUploadBar"), $("calculationUploadText")));
$("refreshReferencesButton")?.addEventListener("click", loadReferences);
$("refreshAdminButton")?.addEventListener("click", async () => {
  await loadDevice();
  await loadReferences();
});
$("referenceSort")?.addEventListener("change", () => {
  referencePage = 1;
  renderReferences(currentReferences);
});
["deviceId", "operatorName"].forEach((id) => {
  $(id)?.addEventListener("change", rememberCurrentMetadata);
  $(id)?.addEventListener("blur", rememberCurrentMetadata);
});

refreshInputSuggestions();
updateManualHoverInfo(null);
renderManualEditor();
loadDevice();
loadReferences();
setInterval(loadDevice, 10000);
