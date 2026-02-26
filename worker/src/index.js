// Cloudflare Worker: cron trigger fetches SFMTA bus data from 511.org,
// parses arrival times, and stores compact results in KV.
// HTTP handler reads from KV — nearly zero CPU time, no 503s.

const ALLOWED_STOPS = {
  "24": new Set(["15147","14429","14330","14315","13521","14143","15882","15878","14624","14428","14331","14314","13520","14142","15881","15490"]),
  "23": new Set(["17208","16436","14386","14192","14200","15882","15864","15865","16453","16435","14387","14198","14203","15881","15863","15776"]),
  "49": new Set(["16819","18102","18104","15836","15552","15566","15572","15614","15782","17804","15926","16820","18091","18089","15546","15551","15565","15571","15613","15783","15781","15791"]),
  "14": new Set(["16498","15529","15536","15543","15836","15552","15566","15572","15614","15592","15588","15693","15530","15535","15542","17299","15551","15565","15571","15613","15593","17099"]),
  "14R": new Set(["16498","15529","15536","15552","15566","15572","15614","15592","15588","15693","15530","15535","15551","15565","15571","15613","15593"]),
  "67": new Set(["17532","17746","14686","17924","14688","14690","13476","17552","14697","13710","14687","14568"]),
  "J": new Set(["17217","16994","16995","16997","16996","18059","16214","18156","16280","14788","15418","16992","15731","15417","15727","15419","14006","16215","18155","16277","14787","17778"]),
};

const MAX_ARRIVALS_PER_STOP = 3;
const KV_KEY = "arrivals";

// In-memory cache — updated by cron, read by fetch (same isolate)
let cachedPayload = null;

// Find closing brace matching the open brace at openPos, skipping JSON strings
function findClosingBrace(text, openPos) {
  let depth = 1;
  let i = openPos + 1;
  const len = text.length;
  while (i < len && depth > 0) {
    const ch = text.charCodeAt(i);
    if (ch === 123) depth++;       // {
    else if (ch === 125) depth--;  // }
    else if (ch === 34) {          // " — skip string contents
      i++;
      while (i < len) {
        if (text.charCodeAt(i) === 92) i++;       // backslash: skip next
        else if (text.charCodeAt(i) === 34) break; // closing quote
        i++;
      }
    }
    i++;
  }
  return i - 1;
}

// Fetch from 511 API, parse, and store in KV
async function refreshData(env) {
  const apiUrl =
    `https://api.511.org/transit/StopMonitoring?api_key=${env.API_KEY}&agency=SF&format=json`;

  const apiResponse = await fetch(apiUrl);
  if (!apiResponse.ok) {
    console.error(`511 API error: ${apiResponse.status}`);
    return;
  }

  let text = await apiResponse.text();
  const cpuStart = Date.now();
  if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);

  const tsMatch = text.match(/"ResponseTimestamp"\s*:\s*"([^"]+)"/);
  const nowMs = tsMatch ? new Date(tsMatch[1]).getTime() : Date.now();

  const result = {};
  const visitArrayKey = '"MonitoredStopVisit"';
  const arrayStart = text.indexOf(visitArrayKey);
  if (arrayStart === -1) {
    console.error("No visits in 511 response");
    return;
  }

  let pos = text.indexOf('[', arrayStart + visitArrayKey.length);
  if (pos === -1) {
    console.error("Malformed visit array");
    return;
  }

  while (pos < text.length) {
    const objStart = text.indexOf('{', pos);
    if (objStart === -1) break;

    const objEnd = findClosingBrace(text, objStart);
    const visitText = text.substring(objStart, objEnd + 1);
    pos = objEnd + 1;

    const lineMatch = visitText.match(/"LineRef"\s*:\s*"([^"]+)"/);
    if (!lineMatch || !ALLOWED_STOPS[lineMatch[1]]) continue;

    const lineRef = lineMatch[1];

    const visit = JSON.parse(visitText);
    const journey = visit.MonitoredVehicleJourney;
    if (!journey) continue;

    const stopRef = journey.MonitoredCall?.StopPointRef;
    if (!stopRef || !ALLOWED_STOPS[lineRef].has(stopRef)) continue;

    const arrival =
      journey.MonitoredCall.ExpectedArrivalTime ||
      journey.MonitoredCall.AimedArrivalTime;
    if (!arrival) continue;

    const minutes = Math.floor((new Date(arrival).getTime() - nowMs) / 60000);
    if (minutes < 0) continue;

    if (!result[lineRef]) result[lineRef] = {};
    if (!result[lineRef][stopRef]) result[lineRef][stopRef] = [];
    result[lineRef][stopRef].push(minutes);
  }

  for (const line in result) {
    for (const stop in result[line]) {
      const arr = result[line][stop];
      arr.sort((a, b) => a - b);
      if (arr.length > MAX_ARRIVALS_PER_STOP) {
        result[line][stop] = arr.slice(0, MAX_ARRIVALS_PER_STOP);
      }
    }
  }

  const cpuMs = Date.now() - cpuStart;
  const payload = JSON.stringify({ data: result, cpuMs, updatedAt: Date.now() });
  cachedPayload = payload;
  await env.BUS_KV.put(KV_KEY, payload);
  console.log(`Refreshed KV — cpuMs: ${cpuMs}, keys: ${Object.keys(result).length}`);
}

export default {
  // HTTP handler: read pre-computed data from KV
  async fetch(request, env) {
    const authHeader = request.headers.get("Authorization") || "";
    const token = authHeader.startsWith("Bearer ") ? authHeader.slice(7) : "";
    if (token !== env.AUTH_TOKEN) {
      return new Response("Unauthorized", { status: 401 });
    }

    // Prefer in-memory cache (updated by cron), fall back to KV
    let raw = cachedPayload;
    if (!raw) {
      raw = await env.BUS_KV.get(KV_KEY, { cacheTtl: 60 });
    }
    if (!raw) {
      console.log("No cached data — doing live fetch to seed");
      await refreshData(env);
      raw = cachedPayload;
    }
    if (!raw) {
      return new Response("Failed to fetch data", { status: 503 });
    }

    const { data, cpuMs, updatedAt } = JSON.parse(raw);
    return new Response(JSON.stringify(data), {
      headers: {
        "Content-Type": "application/json",
        "X-Cpu-Ms": String(cpuMs || 0),
        "X-Updated-At": String(updatedAt || 0),
      },
    });
  },

  // Cron handler: fetch from 511, parse, store in KV
  async scheduled(event, env, ctx) {
    ctx.waitUntil(refreshData(env));
  },
};
