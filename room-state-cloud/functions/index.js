/**
 * Import function triggers from their respective submodules:
 *
 * const {onCall} = require("firebase-functions/v2/https");
 * const {onDocumentWritten} = require("firebase-functions/v2/firestore");
 *
 * See a full list of supported triggers at https://firebase.google.com/docs/functions
 */

const {setGlobalOptions} = require("firebase-functions");
const { onRequest } = require("firebase-functions/v2/https");
const { defineSecret } = require("firebase-functions/params");
const admin = require("firebase-admin");

if (!admin.apps.length) admin.initializeApp();
const db = admin.firestore();

const INGEST_API_KEY = defineSecret("INGEST_API_KEY");

// If you moved to us-west2, keep region here; otherwise use us-central1.
exports.ingestEvent = onRequest(
  { region: "us-west2", secrets: [INGEST_API_KEY] },
  async (req, res) => {
    try {
      if (req.method !== "POST") return res.status(405).send("Use POST");

      const apiKey = process.env.INGEST_API_KEY;
      const key = req.get("x-api-key");
      if (!apiKey || key !== apiKey) return res.status(401).send("Unauthorized");

      const {
        device,            // "blinds" | "desk_led"
        action,            // "TOGGLE" | "OPEN" | "CLOSE" | "ON" | "OFF" | ...
        requested_state,   // optional: "open"/"closed" or "on"/"off"
        confirmed_state,   // optional: "open"/"closed" or "on"/"off"
        tx_id,             // required: idempotency key
        client_ts,         // optional: millis
        source             // optional: "main-ttgo" | "blinds-node" | "led-node"
      } = req.body || {};

      if (!device || !action || !tx_id) {
        return res.status(400).send("Missing required fields");
      }

      const now = admin.firestore.FieldValue.serverTimestamp();

      // Build payload without undefineds
      const eventDoc = {
        device,
        action,
        source: source ?? null,
        client_ts: (typeof client_ts === "number") ? client_ts : null,
        ts: now,
      };
      if (requested_state != null) eventDoc.requested_state = requested_state;
      if (confirmed_state != null) eventDoc.confirmed_state = confirmed_state;

      // Idempotent upsert
      await db.collection("events").doc(tx_id).set(eventDoc, { merge: true });

      // Update latest_state only if we have a concrete state
      const latest = (confirmed_state != null) ? confirmed_state
                    : (requested_state != null) ? requested_state
                    : null;

      if (latest != null) {
        await db.collection("latest_state").doc(device).set({
          state: latest,
          updated_at: now
        }, { merge: true });
      }

      return res.status(200).send({ ok: true });
    } catch (e) {
      console.error(e);
      return res.status(500).send("Server error");
    }
  }
);

const functions = require("firebase-functions/v2");

// For cost control, you can set the maximum number of containers that can be
// running at the same time. This helps mitigate the impact of unexpected
// traffic spikes by instead downgrading performance. This limit is a
// per-function limit. You can override the limit for each function using the
// `maxInstances` option in the function's options, e.g.
// `onRequest({ maxInstances: 5 }, (req, res) => { ... })`.
// NOTE: setGlobalOptions does not apply to functions using the v1 API. V1
// functions should each use functions.runWith({ maxInstances: 10 }) instead.
// In the v1 API, each function can only serve one request per container, so
// this will be the maximum concurrent request count.
setGlobalOptions({ maxInstances: 10 });

// Create and deploy your first functions
// https://firebase.google.com/docs/functions/get-started

// exports.helloWorld = onRequest((request, response) => {
//   logger.info("Hello logs!", {structuredData: true});
//   response.send("Hello from Firebase!");
// });
