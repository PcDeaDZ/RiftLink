/**
 * Абстрактный mesh-фон в духе app/lib/widgets/mesh_background.dart (MeshBackgroundPainter).
 * Статическая сетка кэшируется; импульсы — упрощённые «путешествия» по рёбрам.
 */
const DOT_OPACITY = 0.06;
const LINE_OPACITY = 0.04;
const PULSE_OPACITY = 0.26;
const PULSE_RADIUS = 3.4;
const PULSE_TIME_SCALE = 2.8;
const BASE_SPACING = 48;
const MAX_PULSES = 36;

function stableNoise(p) {
  const v = Math.sin(p.x * 12.9898 + p.y * 78.233) * 43758.5453;
  return v - Math.floor(v);
}

function densityForY(y, height) {
  const yNorm = Math.min(1, Math.max(0, y / height));
  const fadeStart = 0.92;
  const minDensity = 0.86;
  if (yNorm <= fadeStart) return 1;
  const t = (yNorm - fadeStart) / (1 - fadeStart);
  return 1 - (1 - minDensity) * t ** 1.35;
}

function buildPoints(width, height) {
  const allPoints = [];
  let y = 0;
  let row = 0;
  while (y <= height * 2.2) {
    const spacing = BASE_SPACING + (y / height) * 24;
    const cols = Math.floor(width / spacing) + 2;
    for (let c = 0; c < cols; c += 1) {
      const x = c * spacing + (row % 2 === 1 ? spacing * 0.5 : 0);
      if (x <= width + spacing) {
        allPoints.push({ x, y });
      }
    }
    y += spacing * 0.85;
    row += 1;
  }

  const points = [];
  for (const p of allPoints) {
    const density = densityForY(p.y, height);
    if (stableNoise(p) <= density) {
      points.push(p);
    }
  }

  /* Умеренное разрежение при очень плотной сетке — быстрее строим рёбра */
  let sparse = points;
  if (sparse.length > 380) {
    sparse = sparse.filter((_, idx) => idx % 2 === 0);
  }

  return { allPoints, points: sparse };
}

function buildEdges(points, maxDist) {
  const edges = [];
  const n = points.length;
  for (let i = 0; i < n; i += 1) {
    for (let j = i + 1; j < n; j += 1) {
      const dx = points[i].x - points[j].x;
      const dy = points[i].y - points[j].y;
      const dist = Math.hypot(dx, dy);
      if (dist < maxDist) {
        edges.push({ a: points[i], b: points[j], i, j });
      }
    }
  }
  return edges;
}

function pickPulseEdges(edges, width, height) {
  const animEdges = edges.filter((e) => {
    const yMid = (e.a.y + e.b.y) * 0.5;
    const density = densityForY(yMid, height);
    const boostedDensity = yMid > height * 0.76 ? Math.max(density, 0.82) : density;
    const edgeSeed = { x: e.a.x + e.b.x, y: e.a.y + e.b.y };
    return stableNoise(edgeSeed) <= boostedDensity;
  });

  const pool = animEdges.filter(
    (e) =>
      e.a.x >= -8 &&
      e.a.x <= width + 8 &&
      e.b.x >= -8 &&
      e.b.x <= width + 8 &&
      e.a.y >= -8 &&
      e.b.y <= height + 8,
  );

  const pulses = [];
  const step = Math.max(1, Math.floor(pool.length / (MAX_PULSES * 2)));
  for (let k = 0; k < pool.length && pulses.length < MAX_PULSES; k += step) {
    const e = pool[k];
    const seed = { x: e.a.x * 0.71 + e.b.x * 1.31, y: e.a.y * 1.91 + e.b.y * 0.47 };
    if (stableNoise(seed) > 0.45) continue;
    const h = stableNoise(seed);
    const speed = 0.55 + h * 0.85;
    const phase = h * 7 + k * 0.17;
    pulses.push({ a: e.a, b: e.b, speed, phase, bright: pulses.length % 2 === 0 });
  }

  /* Запас: жёсткий фильтр часто оставлял 0 импульсов — добираем из pool */
  if (pulses.length < 12 && pool.length > 0) {
    for (let k = 0; k < pool.length && pulses.length < MAX_PULSES; k += 1) {
      const e = pool[k];
      const seed = { x: e.a.x + k, y: e.a.y + k * 0.13 };
      const h = stableNoise(seed);
      pulses.push({
        a: e.a,
        b: e.b,
        speed: 0.85 + h * 0.55,
        phase: h * 9.2 + k * 0.31,
        bright: pulses.length % 2 === 0,
      });
    }
  }

  if (pulses.length === 0 && edges.length > 0) {
    for (let k = 0; k < edges.length && pulses.length < 24; k += 1) {
      const e = edges[k];
      const h = stableNoise({ x: e.a.x * 2 + k, y: e.b.y * 2 });
      pulses.push({
        a: e.a,
        b: e.b,
        speed: 1.1 + h * 0.6,
        phase: h * 11 + k * 0.2,
        bright: k % 2 === 0,
      });
    }
  }

  return pulses;
}

function drawStaticMesh(ctx, width, height, points, allPoints, edges) {
  const dotPaint = `rgba(61, 139, 253, ${DOT_OPACITY})`;
  const linePaint = `rgba(61, 139, 253, ${LINE_OPACITY})`;

  for (const p of points) {
    ctx.beginPath();
    ctx.arc(p.x, p.y, 1.2, 0, Math.PI * 2);
    ctx.fillStyle = dotPaint;
    ctx.fill();
  }

  const tailPaint = "rgba(61, 139, 253, 0.06)";
  for (const p of allPoints) {
    if (p.y < height * 0.68 || p.y > height * 1.1) continue;
    const h = stableNoise({ x: p.x + 19, y: p.y + 47 });
    if (h <= 0.34) {
      ctx.beginPath();
      ctx.arc(p.x, p.y, 1.05, 0, Math.PI * 2);
      ctx.fillStyle = tailPaint;
      ctx.fill();
    }
  }

  ctx.strokeStyle = linePaint;
  ctx.lineWidth = 0.8;
  for (const e of edges) {
    ctx.beginPath();
    ctx.moveTo(e.a.x, e.a.y);
    ctx.lineTo(e.b.x, e.b.y);
    ctx.stroke();
  }
}

/**
 * Добираем импульсы только по уже нарисованным рёбрам графа — без «диагоналей в пустоте».
 */
function ensureSyntheticPulses(pulses, edges, height) {
  if (pulses.length >= 8 || edges.length === 0) return;
  const n = 12 - pulses.length;
  const step = Math.max(1, Math.floor(edges.length / Math.max(n * 3, 8)));
  for (let i = 0; i < n; i += 1) {
    const k = (i * step * 7 + Math.floor(edges.length * 0.21)) % edges.length;
    const e = edges[k];
    const yMid = (e.a.y + e.b.y) * 0.5;
    if (yMid < -8 || yMid > height + 8) continue;
    const seed = { x: e.a.x * 0.71 + e.b.x * 1.31, y: e.a.y * 1.91 + e.b.y * 0.47 };
    const h = stableNoise(seed);
    pulses.push({
      a: e.a,
      b: e.b,
      speed: 0.55 + h * 0.85,
      phase: h * 7 + i * 0.17,
      bright: pulses.length % 2 === 0,
    });
  }
}

function initMeshBackground(canvas) {
  if (!canvas || !(canvas instanceof HTMLCanvasElement)) return () => {};

  /** Декоративный фон: при reduce — только слабее/медленнее, не «выключатель» */
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const motionScale = reducedMotion ? 0.55 : 1;
  const timeScale = reducedMotion ? 1.4 : PULSE_TIME_SCALE;

  const ctxMain = canvas.getContext("2d", { alpha: true });
  if (!ctxMain) return () => {};

  let offscreen = null;
  let pulses = [];
  let raf = 0;
  let w = 0;
  let h = 0;
  let dpr = 1;
  let running = true;

  function viewportSize() {
    const iw = Math.max(1, window.innerWidth || 0);
    const sh = document.documentElement.scrollHeight || 0;
    const ih = window.innerHeight || 0;
    const ch = document.documentElement.clientHeight || 0;
    const h0 = Math.max(sh, ih, ch, 1);
    return { w: iw, h: h0 };
  }

  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    const vp = viewportSize();
    w = vp.w;
    h = vp.h;
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
    canvas.style.width = `${w}px`;
    canvas.style.height = `${h}px`;

    ctxMain.setTransform(dpr, 0, 0, dpr, 0, 0);

    const { allPoints, points } = buildPoints(w, h);
    const maxDist = (BASE_SPACING + 24) * 1.5;
    const edges = buildEdges(points, maxDist);

    offscreen = document.createElement("canvas");
    offscreen.width = canvas.width;
    offscreen.height = canvas.height;
    const offCtx = offscreen.getContext("2d");
    if (!offCtx) return;
    offCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
    offCtx.clearRect(0, 0, w, h);
    drawStaticMesh(offCtx, w, h, points, allPoints, edges);

    pulses = pickPulseEdges(edges, w, h);
    ensureSyntheticPulses(pulses, edges, h);
  }

  function drawPulsesWithMotion(ctx, t, pulseList) {
    const soft = `rgba(61, 139, 253, ${PULSE_OPACITY * 0.88 * motionScale})`;
    const bright = `rgba(120, 200, 255, ${Math.min(0.95, PULSE_OPACITY * 1.35 * motionScale)})`;
    const tt = t * timeScale;
    for (const p of pulseList) {
      const u = (Math.sin(tt * p.speed + p.phase) * 0.5 + 0.5) ** 1.05;
      const x = p.a.x + (p.b.x - p.a.x) * u;
      const y = p.a.y + (p.b.y - p.a.y) * u;
      ctx.beginPath();
      ctx.arc(x, y, PULSE_RADIUS * (p.bright ? 1.1 : 0.95) * (0.85 + 0.15 * motionScale), 0, Math.PI * 2);
      ctx.fillStyle = p.bright ? bright : soft;
      ctx.fill();
    }
  }

  function frame(timeMs) {
    if (!running) return;
    if (!offscreen || w < 1) {
      if (!document.hidden) raf = requestAnimationFrame(frame);
      return;
    }
    ctxMain.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctxMain.clearRect(0, 0, w, h);
    ctxMain.drawImage(offscreen, 0, 0);
    if (pulses.length > 0) {
      const t = timeMs * 0.001;
      drawPulsesWithMotion(ctxMain, t, pulses);
    }
    if (running && !document.hidden) {
      raf = requestAnimationFrame(frame);
    }
  }

  function onVisibility() {
    if (document.hidden) {
      cancelAnimationFrame(raf);
    } else if (running) {
      raf = requestAnimationFrame(frame);
    }
  }

  function boot() {
    resize();
    raf = requestAnimationFrame(frame);
  }

  requestAnimationFrame(boot);

  let ro = null;
  if (typeof ResizeObserver !== "undefined") {
    ro = new ResizeObserver(() => {
      resize();
    });
    ro.observe(document.documentElement);
  }
  window.addEventListener("resize", resize, { passive: true });
  document.addEventListener("visibilitychange", onVisibility);

  return function destroy() {
    running = false;
    cancelAnimationFrame(raf);
    window.removeEventListener("resize", resize);
    document.removeEventListener("visibilitychange", onVisibility);
    if (ro) ro.disconnect();
  };
}

const el = document.getElementById("meshCanvas");
if (el) {
  initMeshBackground(el);
}
