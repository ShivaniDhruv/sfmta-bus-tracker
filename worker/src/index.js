// Cloudflare Worker: fetches SFMTA bus data from 511.org,
// filters for allowed lines/stops, and returns compact arrival times.

// Full allowed stops list (mirrors the Arduino allowedStops config)
const ALLOWED_STOPS = {
  "24": ["15147", "14429", "14330", "14315", "13521", "14143", "15882", "15878", "14624", "14428", "14331", "14314", "13520", "14142", "15881", "15490"],
  "23": ["17208", "16436", "14386", "14192", "14200", "15882", "15864", "15865", "16453", "16435", "14387", "14198", "14203", "15881", "15863", "15776"],
  "49": ["16819", "18102", "18104", "15836", "15552", "15566", "15572", "15614", "15782", "17804", "15926", "16820", "18091", "18089", "15546", "15551", "15565", "15571", "15613", "15783", "15781", "15791"],
  "14": ["16498", "15529", "15536", "15543", "15836", "15552", "15566", "15572", "15614", "15592", "15588", "15693", "15530", "15535", "15542", "17299", "15551", "15565", "15571", "15613", "15593", "17099"],
  "14R": ["16498", "15529", "15536", "15552", "15566", "15572", "15614", "15592", "15588", "15693", "15530", "15535", "15551", "15565", "15571", "15613", "15593"],
  "67": ["17532", "17746", "14686", "17924", "14688", "14690", "13476", "17552", "14697", "13710", "14687", "14568"],
  "J": ["17217", "16994", "16995", "16997", "16996", "18059", "16214", "18156", "16280", "14788", "15418", "16992", "15731", "15417", "15727", "15419", "14006", "16215", "18155", "16277", "14787", "17778"],
};

// Build a fast lookup set: "lineRef:stopRef"
const ALLOWED_SET = new Set();
for (const lineRef in ALLOWED_STOPS) {
  for (const stopRef of ALLOWED_STOPS[lineRef]) {
    ALLOWED_SET.add(`${lineRef}:${stopRef}`);
  }
}

const MAX_ARRIVALS_PER_STOP = 3;

export default {
  async fetch(request, env) {
    // Simple auth check
    const url = new URL(request.url);
    const token = url.searchParams.get("token");
    if (token !== env.AUTH_TOKEN) {
      return new Response("Unauthorized", { status: 401 });
    }

    try {
      const apiUrl =
        `https://api.511.org/transit/StopMonitoring?api_key=${env.API_KEY}&agency=SF&format=json`;

      const apiResponse = await fetch(apiUrl);
      if (!apiResponse.ok) {
        return new Response(`511 API error: ${apiResponse.status}`, { status: 502 });
      }

      const data = await apiResponse.json();
      const delivery = data?.ServiceDelivery?.StopMonitoringDelivery;
      if (!delivery) {
        return new Response("Unexpected API response structure", { status: 502 });
      }

      const responseTimestamp = delivery.ResponseTimestamp || data.ServiceDelivery.ResponseTimestamp;
      const nowMs = new Date(responseTimestamp).getTime();
      const visits = delivery.MonitoredStopVisit || [];

      // Build result: { "24": { "14143": [5, 12], ... }, ... }
      const result = {};

      for (const visit of visits) {
        const journey = visit?.MonitoredVehicleJourney;
        if (!journey) continue;

        const lineRef = journey.LineRef;
        const stopRef = journey.MonitoredCall?.StopPointRef;
        const expectedArrival =
          journey.MonitoredCall?.ExpectedArrivalTime ||
          journey.MonitoredCall?.AimedArrivalTime;

        if (!lineRef || !stopRef || !expectedArrival) continue;
        if (!ALLOWED_SET.has(`${lineRef}:${stopRef}`)) continue;

        const arrivalMs = new Date(expectedArrival).getTime();
        const minutes = Math.floor((arrivalMs - nowMs) / 60000);
        if (minutes < 0) continue;

        if (!result[lineRef]) result[lineRef] = {};
        if (!result[lineRef][stopRef]) result[lineRef][stopRef] = [];

        result[lineRef][stopRef].push(minutes);
      }

      // Sort and trim to MAX_ARRIVALS_PER_STOP
      for (const lineRef in result) {
        for (const stopRef in result[lineRef]) {
          const arr = result[lineRef][stopRef];
          arr.sort((a, b) => a - b);
          if (arr.length > MAX_ARRIVALS_PER_STOP) {
            result[lineRef][stopRef] = arr.slice(0, MAX_ARRIVALS_PER_STOP);
          }
        }
      }

      return new Response(JSON.stringify(result), {
        headers: { "Content-Type": "application/json" },
      });
    } catch (err) {
      return new Response(`Error: ${err.message}`, { status: 500 });
    }
  },
};
