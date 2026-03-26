/**
 * Абстрактный mesh-фон в духе app/lib/widgets/mesh_background.dart (MeshBackgroundPainter).
 * Статическая сетка кэшируется; импульсы идут по цепочке рёбер (pickNextHop), как во Flutter.
 */
const DOT_OPACITY = 0.06;
const LINE_OPACITY = 0.04;
const PULSE_OPACITY = 0.26;
const PULSE_RADIUS = 3.4;
const BASE_SPACING = 48;
/** Как во Flutter: не больше импульсов за кадр */
const MAX_DRAWN_PULSES = 48;

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
        const pi = points[i];
        const pj = points[j];
        const a = pi.y < pj.y || (pi.y === pj.y && pi.x <= pj.x) ? pi : pj;
        const b = pi.y < pj.y || (pi.y === pj.y && pi.x <= pj.x) ? pj : pi;
        edges.push({ a, b, i, j });
      }
    }
  }
  return edges;
}

/** Те же фильтры, что у animEdges во Flutter (animZoneFraction = 1). */
function buildAnimEdges(edges, width, height) {
  const animLimitY = height;
  return edges
    .filter((e) => e.a.y < animLimitY && e.b.y < animLimitY)
    .filter(
      (e) =>
        e.a.x >= -8 &&
        e.a.x <= width + 8 &&
        e.b.x >= -8 &&
        e.b.x <= width + 8 &&
        e.a.y >= -8 &&
        e.a.y <= height + 8 &&
        e.b.y >= -8 &&
        e.b.y <= height + 8,
    )
    .filter((e) => {
      const yMid = (e.a.y + e.b.y) * 0.5;
      const density = densityForY(yMid, height);
      const boostedDensity = yMid > height * 0.76 ? Math.max(density, 0.82) : density;
      const edgeSeed = { x: e.a.x + e.b.x, y: e.a.y + e.b.y };
      return stableNoise(edgeSeed) <= boostedDensity;
    });
}

function pointKey(p) {
  return `${p.x.toFixed(2)}:${p.y.toFixed(2)}`;
}

function buildNeighborMap(animEdges) {
  const byPoint = {};
  for (const e of animEdges) {
    const ka = pointKey(e.a);
    const kb = pointKey(e.b);
    if (!byPoint[ka]) byPoint[ka] = [];
    if (!byPoint[kb]) byPoint[kb] = [];
    byPoint[ka].push(e.b);
    byPoint[kb].push(e.a);
  }
  return byPoint;
}

function pickNextHop(prev, cur, selector, preferDown, byPoint) {
  const prevKey = pointKey(prev);
  const all = (byPoint[pointKey(cur)] || []).filter((n) => pointKey(n) !== prevKey);
  if (all.length === 0) return prev;
  const directed = preferDown
    ? all.filter((n) => n.y >= cur.y + 0.5)
    : all.filter((n) => n.y <= cur.y - 0.5);
  const pool = directed.length > 0 ? directed : all;
  if (preferDown && pool.length > 1) {
    pool.sort((a, b) => b.y - a.y);
    const topCount = Math.max(1, Math.ceil(pool.length * 0.85));
    const top = pool.slice(0, topCount);
    const idx = Math.min(Math.floor(selector * top.length), top.length - 1);
    return top[idx];
  }
  const idx = Math.min(Math.floor(selector * pool.length), pool.length - 1);
  return pool[idx];
}

/** Конфиги импульсов: то же разрежение и двойные полосы, что во Flutter. */
function buildPulseConfigs(animEdges) {
  const configs = [];
  for (let ei = 0; ei < animEdges.length; ei += 1) {
    const edge = animEdges[ei];
    const seed = {
      x: edge.a.x * 0.71 + edge.b.x * 1.31,
      y: edge.a.y * 1.91 + edge.b.y * 0.47,
    };
    const h = stableNoise(seed);
    if (h > 0.22) continue;

    const laneCount = h < 0.1 ? 2 : 1;
    for (let lane = 0; lane < laneCount; lane += 1) {
      const laneSeed = { x: seed.x + lane * 13.7, y: seed.y + lane * 9.1 };
      const lh = stableNoise(laneSeed);
      configs.push({
        edge: { a: edge.a, b: edge.b },
        seed,
        h,
        lane,
        lh,
        bright: lane % 2 === 0,
      });
      if (configs.length >= MAX_DRAWN_PULSES) return configs;
    }
  }
  return configs;
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

function initMeshBackground(canvas) {
  if (!canvas || !(canvas instanceof HTMLCanvasElement)) return () => {};

  /** Декоративный фон: при reduce — только слабее/медленнее, не «выключатель» */
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const motionScale = reducedMotion ? 0.55 : 1;

  const ctxMain = canvas.getContext("2d", { alpha: true });
  if (!ctxMain) return () => {};

  let offscreen = null;
  let pulses = [];
  /** Смежность по вершинам для pickNextHop (как byPoint во Flutter) */
  let pulseByPoint = null;
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

    const animEdges = buildAnimEdges(edges, w, h);
    pulseByPoint = animEdges.length > 0 ? buildNeighborMap(animEdges) : null;
    pulses = buildPulseConfigs(animEdges);
  }

  function selectorFor(seed, lane, turn, salt) {
    return stableNoise({
      x: seed.x * 0.17 + lane * 11.0 + turn * 0.73 + salt * 7.0,
      y: seed.y * 0.23 + lane * 13.0 + turn * 1.11 + salt * 5.0,
    });
  }

  /**
   * Цепочка сегментов A→B→…→H и обратно, как в MeshBackgroundPainter (14 шагов на цикл).
   */
  function drawPulsesWithMotion(ctx, timeSec, pulseList, byPoint, height) {
    if (!byPoint || pulseList.length === 0) return;
    const soft = `rgba(61, 139, 253, ${PULSE_OPACITY * 0.88 * motionScale})`;
    const bright = `rgba(120, 200, 255, ${Math.min(0.95, PULSE_OPACITY * 1.35 * motionScale)})`;
    for (const cfg of pulseList) {
      const { edge, seed, h, lane, lh, bright: isBright } = cfg;
      const speed = 0.018 + lh * 0.05;
      const phase = h * 7.0 + lh * 13.0 + lane * 0.61;
      /* Как во Flutter: v = timeSec * speed + phase; замедление только при reduce motion */
      const v = timeSec * speed * motionScale + phase;
      const turn = Math.floor(v);
      const cycle = (v - turn) * 14.0;
      const seg = Math.floor(cycle);
      const u = cycle - seg;

      const a = edge.a;
      const b = edge.b;
      const c = pickNextHop(a, b, selectorFor(seed, lane, turn, 1), true, byPoint);
      const d = pickNextHop(b, c, selectorFor(seed, lane, turn, 2), true, byPoint);
      const e = pickNextHop(c, d, selectorFor(seed, lane, turn, 3), true, byPoint);
      const f = pickNextHop(d, e, selectorFor(seed, lane, turn, 4), true, byPoint);
      const g = pickNextHop(e, f, selectorFor(seed, lane, turn, 5), true, byPoint);
      const h2 = pickNextHop(f, g, selectorFor(seed, lane, turn, 6), true, byPoint);

      let from;
      let to;
      switch (seg) {
        case 0:
          from = a;
          to = b;
          break;
        case 1:
          from = b;
          to = c;
          break;
        case 2:
          from = c;
          to = d;
          break;
        case 3:
          from = d;
          to = e;
          break;
        case 4:
          from = e;
          to = f;
          break;
        case 5:
          from = f;
          to = g;
          break;
        case 6:
          from = g;
          to = h2;
          break;
        case 7:
          from = h2;
          to = g;
          break;
        case 8:
          from = g;
          to = f;
          break;
        case 9:
          from = f;
          to = e;
          break;
        case 10:
          from = e;
          to = d;
          break;
        case 11:
          from = d;
          to = c;
          break;
        case 12:
          from = c;
          to = b;
          break;
        default:
          from = b;
          to = a;
          break;
      }

      const xRaw = from.x + (to.x - from.x) * u;
      const yRaw = from.y + (to.y - from.y) * u;
      const y = Math.max(-8, Math.min(height + 8, yRaw));
      const radius = PULSE_RADIUS * (0.9 + lh * 0.45) * (0.85 + 0.15 * motionScale);
      ctx.beginPath();
      ctx.arc(xRaw, y, radius, 0, Math.PI * 2);
      ctx.fillStyle = isBright ? bright : soft;
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
    if (pulses.length > 0 && pulseByPoint) {
      const timeSec = Date.now() / 1000;
      drawPulsesWithMotion(ctxMain, timeSec, pulses, pulseByPoint, h);
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
