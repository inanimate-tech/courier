// Gemini speech-to-speech relay — per-device Durable Object that bridges
// the device's WebSocket to a Gemini Live API WebSocket.
//
// Protocol (device-facing):
//   client → server
//     {"type":"start"}                          // button pressed
//     <binary int16 PCM @ 16kHz>                // mic audio
//     {"type":"stop"}                           // button released
//
//   server → client
//     {"type":"session", "chatId":"..."}        // burst: frame 1/3
//     {"type":"ready"}                          // burst: frame 2/3
//     {"type":"settings", "volume":180}         // burst: frame 3/3
//     {"type":"transcript", "source":"user"|"model", "text":"..."}
//     {"type":"turn_complete"}
//     {"type":"drop_audio"}                     // interrupted
//     <binary int16 PCM @ 24kHz>                // Gemini audio

interface GeminiMessage {
	setupComplete?: Record<string, unknown>;
	serverContent?: {
		modelTurn?: { parts?: Array<{ inlineData?: { mimeType: string; data: string } }> };
		turnComplete?: boolean;
		interrupted?: boolean;
		inputTranscription?: { text: string };
		outputTranscription?: { text: string };
	};
}

const GEMINI_MODEL = 'models/gemini-3.1-flash-live-preview';
const GEMINI_WS_URL =
	'https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent';

const SYSTEM_PROMPT = [
	'You are a voice assistant running on a handheld M5Stick — a small',
	'ESP32 device with a tiny speaker. Keep replies short, warm, and',
	'conversational. If you cannot hear the user clearly, say so briefly',
	'rather than guessing.',
].join(' ');

export class LiveSession {
	private state: DurableObjectState;
	private env: Env;
	private deviceWs: WebSocket | null = null;
	private geminiWs: WebSocket | null = null;
	private geminiReady = false;
	private chatId = '';

	constructor(state: DurableObjectState, env: Env) {
		this.state = state;
		this.env = env;
	}

	async fetch(request: Request): Promise<Response> {
		if (request.headers.get('Upgrade') !== 'websocket') {
			return new Response('Expected WebSocket', { status: 426 });
		}

		this.chatId = new URL(request.url).searchParams.get('chat_id') ?? crypto.randomUUID();

		const pair = new WebSocketPair();
		const [client, server] = Object.values(pair);
		server.accept();
		this.deviceWs = server;

		// Deliberate burst: three JSON frames in one scheduler slice, sent
		// before the Gemini WS even opens. This is the reason Courier's
		// transport uses a FIFO — on the device side, all three need to
		// reach onMessage() without any being dropped.
		this.sendToDevice({ type: 'session', chatId: this.chatId });

		// Bind cleanup to this specific socket. If a new connection arrives
		// before the old one's close fires, the stale listener must not tear
		// down the replacement.
		server.addEventListener('message', (event) => this.onDeviceMessage(event.data));
		server.addEventListener('close', () => { if (this.deviceWs === server) this.cleanup(); });
		server.addEventListener('error', () => { if (this.deviceWs === server) this.cleanup(); });

		await this.connectGemini();

		return new Response(null, { status: 101, webSocket: client });
	}

	private async connectGemini() {
		const resp = await fetch(`${GEMINI_WS_URL}?key=${this.env.GEMINI_API_KEY}`, {
			headers: { Upgrade: 'websocket' },
		});

		const ws = resp.webSocket;
		if (!ws) {
			console.error('[Gemini] upgrade failed');
			this.deviceWs?.close(1011, 'gemini unavailable');
			return;
		}
		ws.accept();
		this.geminiWs = ws;

		ws.addEventListener('message', (event) => {
			if (this.geminiWs !== ws) return;  // stale
			const text =
				typeof event.data === 'string'
					? event.data
					: new TextDecoder().decode(event.data as ArrayBuffer);
			try {
				this.onGeminiMessage(JSON.parse(text) as GeminiMessage);
			} catch {
				// ignore non-JSON frames
			}
		});
		ws.addEventListener('close', () => {
			if (this.geminiWs !== ws) return;  // stale
			this.geminiWs = null;
			this.geminiReady = false;
			// If Gemini dropped (idle timeout, transient upstream failure),
			// tear the device WS down too so Courier reconnects into a
			// fresh session — otherwise every later mic chunk is silently
			// discarded while geminiReady is false.
			if (this.deviceWs) {
				try { this.deviceWs.close(1011, 'gemini disconnected'); } catch { /* ignore */ }
			}
		});

		ws.send(
			JSON.stringify({
				setup: {
					model: GEMINI_MODEL,
					generationConfig: { responseModalities: ['AUDIO'] },
					inputAudioTranscription: {},
					outputAudioTranscription: {},
					systemInstruction: { parts: [{ text: SYSTEM_PROMPT }] },
				},
			})
		);
	}

	private onDeviceMessage(data: string | ArrayBuffer) {
		if (data instanceof ArrayBuffer) {
			if (!this.geminiReady || !this.geminiWs) return;
			this.geminiWs.send(
				JSON.stringify({
					realtimeInput: {
						audio: {
							data: arrayBufferToBase64(data),
							mimeType: 'audio/pcm;rate=16000',
						},
					},
				})
			);
			return;
		}

		let msg: { type?: string };
		try {
			msg = JSON.parse(data);
		} catch {
			return;
		}

		// Flush 1s of trailing silence so Gemini's VAD detects end-of-turn.
		if (msg.type === 'stop' && this.geminiWs && this.geminiReady) {
			const silence = new ArrayBuffer(32000);  // 16kHz * 1s * 2 bytes
			this.geminiWs.send(
				JSON.stringify({
					realtimeInput: {
						audio: { data: arrayBufferToBase64(silence), mimeType: 'audio/pcm;rate=16000' },
					},
				})
			);
		}
	}

	private onGeminiMessage(msg: GeminiMessage) {
		if (msg.setupComplete) {
			this.geminiReady = true;
			// Second and third frames of the burst. Sent back-to-back —
			// these land in the same WS task tick on the device and all
			// three must survive the FIFO.
			this.sendToDevice({ type: 'ready' });
			this.sendToDevice({ type: 'settings', volume: 180 });
			return;
		}

		const sc = msg.serverContent;
		if (!sc) return;

		for (const part of sc.modelTurn?.parts ?? []) {
			if (part.inlineData?.data) {
				this.deviceWs?.send(base64ToArrayBuffer(part.inlineData.data));
			}
		}

		if (sc.inputTranscription?.text) {
			this.sendToDevice({ type: 'transcript', source: 'user', text: sc.inputTranscription.text });
		}
		if (sc.outputTranscription?.text) {
			this.sendToDevice({ type: 'transcript', source: 'model', text: sc.outputTranscription.text });
		}
		if (sc.interrupted) {
			this.sendToDevice({ type: 'drop_audio' });
		}
		if (sc.turnComplete) {
			this.sendToDevice({ type: 'turn_complete' });
		}
	}

	private sendToDevice(msg: Record<string, unknown>) {
		this.deviceWs?.send(JSON.stringify(msg));
	}

	private cleanup() {
		if (this.geminiWs) {
			try { this.geminiWs.close(); } catch { /* ignore */ }
			this.geminiWs = null;
		}
		this.geminiReady = false;
		this.deviceWs = null;
	}
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url);
		if (url.pathname !== '/ws') {
			return new Response('Not found', { status: 404 });
		}
		// One DO per device so reconnects land back in the same session.
		const deviceId = url.searchParams.get('device_id') ?? 'anon';
		const id = env.LIVE_SESSION.idFromName(deviceId);
		return env.LIVE_SESSION.get(id).fetch(request);
	},
} satisfies ExportedHandler<Env>;

// ─── utils ───

function arrayBufferToBase64(buffer: ArrayBuffer): string {
	const bytes = new Uint8Array(buffer);
	const CHUNK = 8192;
	let binary = '';
	for (let i = 0; i < bytes.length; i += CHUNK) {
		binary += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
	}
	return btoa(binary);
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64);
	const bytes = new Uint8Array(binary.length);
	for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
	return bytes.buffer;
}
