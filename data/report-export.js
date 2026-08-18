// Экспорт одного отчёта в тот же CSV-формат, который принимает
// ручной конструктор эталона в admin.js.
// Загружается после app.js и переопределяет exportCalculationReportCsv().

(function () {
  function pairKey(a, b) {
    const left = Math.min(Number(a), Number(b));
    const right = Math.max(Number(a), Number(b));
    return `${left}:${right}`;
  }

  function validPair(pair) {
    const a = Number(pair?.a ?? pair?.source);
    const b = Number(pair?.b ?? pair?.target);
    return Number.isInteger(a) && Number.isInteger(b) && a >= 0 && b >= 0 && a <= 1023 && b <= 1023 && a !== b;
  }

  function pairSet(items) {
    return new Set((Array.isArray(items) ? items : [])
      .filter(validPair)
      .map((item) => pairKey(item?.a ?? item?.source, item?.b ?? item?.target)));
  }

  function endpointMapFromAnnotations(annotations) {
    const map = new Map();

    const remember = (pin, mark, localPin) => {
      const numericPin = Number(pin);
      if (!Number.isInteger(numericPin) || numericPin < 0 || numericPin > 1023) return;
      const markText = String(mark ?? "").trim();
      const localText = String(localPin ?? "").trim();
      if (!markText || !localText) return;
      if (!map.has(numericPin)) map.set(numericPin, { mark: markText, localPin: localText });
    };

    for (const item of Array.isArray(annotations) ? annotations : []) {
      remember(item?.a, item?.markA, item?.localPinA);
      remember(item?.b, item?.markB, item?.localPinB);
    }

    return map;
  }

  function endpointFor(pair, side, pin, endpointMap) {
    const mark = String(pair?.[`mark${side}`] ?? "").trim();
    const localPin = String(pair?.[`localPin${side}`] ?? "").trim();
    const known = endpointMap.get(Number(pin));

    return {
      mark: mark || known?.mark || "КСК",
      localPin: localPin || known?.localPin || String(Number(pin) + 1)
    };
  }

  function splitPositionFromNote(value) {
    const note = String(value ?? "").trim();
    const match = note.match(/^позиция\s*:\s*([^;]+)(?:;\s*(.*))?$/i);
    if (!match) return { position: "", note };
    return {
      position: String(match[1] || "").trim(),
      note: String(match[2] || "").trim()
    };
  }

  function mergePairMetadata(pair, annotationMap) {
    if (!validPair(pair)) return pair;
    const a = Number(pair?.a ?? pair?.source);
    const b = Number(pair?.b ?? pair?.target);
    const annotation = annotationMap.get(pairKey(a, b));
    return annotation ? { ...pair, ...annotation } : pair;
  }

  function buildAnnotationMap(annotations) {
    const map = new Map();
    for (const item of Array.isArray(annotations) ? annotations : []) {
      if (!validPair(item)) continue;
      map.set(pairKey(item.a, item.b), item);
    }
    return map;
  }

  function appendReportRow(rows, pair, endpointMap, statusText) {
    if (!validPair(pair)) return;

    const a = Number(pair?.a ?? pair?.source);
    const b = Number(pair?.b ?? pair?.target);
    const endpointA = endpointFor(pair, "A", a, endpointMap);
    const endpointB = endpointFor(pair, "B", b, endpointMap);
    const parsedNote = splitPositionFromNote(pair?.note);
    const notes = [];

    if (parsedNote.note) notes.push(parsedNote.note);
    if (statusText) notes.push(`Результат: ${statusText}`);

    rows.push([
      String(pair?.wireName ?? "").trim(),
      parsedNote.position,
      endpointA.mark,
      endpointA.localPin,
      endpointB.mark,
      endpointB.localPin,
      notes.join("; ")
    ]);
  }

  // Функция объявлена в app.js как глобальная function declaration.
  // Здесь намеренно заменяем только её реализацию.
  exportCalculationReportCsv = async function exportCalculationReportCsvCompatible(fileName) {
    try {
      const result = await api(`/api/result?file=${encodeURIComponent(fileName)}`);
      const referenceFile = String(result?.reference || "").trim();
      if (!referenceFile) throw new Error("В отчёте не указан файл эталона.");

      const [resultView, referenceView] = await Promise.all([
        api(`/api/result/view?file=${encodeURIComponent(fileName)}`),
        api(`/api/reference/view?file=${encodeURIComponent(referenceFile)}`)
      ]);

      const annotations = Array.isArray(referenceView?.annotations) ? referenceView.annotations : [];
      const annotationMap = buildAnnotationMap(annotations);
      const endpointMap = endpointMapFromAnnotations(annotations);

      // connections в resultView строится непосредственно из сохранённой измеренной матрицы:
      // пара попадает сюда только если A->B и B->A реально обнаружены.
      const measuredPairs = Array.isArray(resultView?.connections) ? resultView.connections : [];
      const measuredKeys = pairSet(measuredPairs);

      // Асимметрию берём из представления матрицы и дублируем первичным списком из JSON отчёта.
      const asymmetricPairs = [
        ...(Array.isArray(resultView?.asymmetricConnections) ? resultView.asymmetricConnections : []),
        ...(Array.isArray(result?.asymmetric) ? result.asymmetric : [])
      ];
      const asymmetricKeys = pairSet(asymmetricPairs);

      // Основой CSV является весь эталон. Норма определяется не по списку missing,
      // а только по факту наличия двусторонней пары в измеренной матрице.
      const referencePairsSource = Array.isArray(referenceView?.connections) && referenceView.connections.length
        ? referenceView.connections
        : annotations;
      const expectedKeys = pairSet(referencePairsSource);

      const rows = [[
        "Провод",
        "Позиция",
        "Маркировка",
        "Контакт",
        "Маркировка",
        "Контакт",
        "Примечание"
      ]];

      const emitted = new Set();
      let computedMissing = 0;
      let computedExtra = 0;
      let computedAsymmetric = 0;

      for (const rawPair of referencePairsSource) {
        if (!validPair(rawPair)) continue;
        const pair = mergePairMetadata(rawPair, annotationMap);
        const a = Number(pair?.a ?? pair?.source);
        const b = Number(pair?.b ?? pair?.target);
        const key = pairKey(a, b);
        if (emitted.has(key)) continue;
        emitted.add(key);

        const statuses = [];
        if (!measuredKeys.has(key)) {
          statuses.push("Обрыв");
          computedMissing += 1;
        }
        if (asymmetricKeys.has(key)) {
          statuses.push("Асимметрия");
          computedAsymmetric += 1;
        }
        if (!statuses.length) statuses.push("Норма");

        appendReportRow(rows, pair, endpointMap, statuses.join("; "));
      }

      // Любая измеренная двусторонняя связь, которой нет в эталоне, является паразитной.
      for (const rawPair of measuredPairs) {
        if (!validPair(rawPair)) continue;
        const a = Number(rawPair?.a ?? rawPair?.source);
        const b = Number(rawPair?.b ?? rawPair?.target);
        const key = pairKey(a, b);
        if (expectedKeys.has(key) || emitted.has(key)) continue;

        emitted.add(key);
        computedExtra += 1;
        const pair = mergePairMetadata(rawPair, annotationMap);
        const statuses = ["Паразитная связь"];
        if (asymmetricKeys.has(key)) {
          statuses.push("Асимметрия");
          computedAsymmetric += 1;
        }
        appendReportRow(rows, pair, endpointMap, statuses.join("; "));
      }

      // Односторонняя связь не входит в measuredPairs, поэтому добавляем неизвестные
      // асимметричные пары отдельно, если они ещё не были строкой эталона.
      for (const rawPair of asymmetricPairs) {
        if (!validPair(rawPair)) continue;
        const a = Number(rawPair?.a ?? rawPair?.source);
        const b = Number(rawPair?.b ?? rawPair?.target);
        const key = pairKey(a, b);
        if (emitted.has(key)) continue;
        emitted.add(key);
        computedAsymmetric += 1;
        appendReportRow(rows, mergePairMetadata(rawPair, annotationMap), endpointMap, "Асимметрия");
      }

      const exportName = String(fileName || "report.json").replace(/\.json$/i, ".csv");
      downloadTextFile(
        exportName,
        rows.map((row) => row.map(csvEscape).join(";")).join("\n"),
        "text/csv;charset=utf-8"
      );

      const count = Math.max(0, rows.length - 1);
      const savedSummary = result?.summary || {};
      const savedMissing = Number(savedSummary.missingLinks || 0);
      const savedExtra = Number(savedSummary.extraLinks || 0);
      const savedAsymmetric = Number(savedSummary.asymmetricLinks || 0);
      const mismatch = savedMissing !== computedMissing ||
        savedExtra !== computedExtra ||
        savedAsymmetric !== computedAsymmetric;

      if (!count) {
        toast("CSV создан, но в выбранном эталоне и результате нет ни одной связи.");
      } else if (mismatch) {
        toast(`CSV выгружен: ${count} строк. Внимание: матрица даёт обрывов ${computedMissing}, паразитных ${computedExtra}, асимметрий ${computedAsymmetric}; сводка отчёта: ${savedMissing}/${savedExtra}/${savedAsymmetric}.`);
      } else {
        toast(`CSV отчёта выгружен: ${count} строк; обрывов ${computedMissing}, паразитных ${computedExtra}, асимметрий ${computedAsymmetric}.`);
      }
    } catch (error) {
      toast(error.message);
    }
  };
})();
