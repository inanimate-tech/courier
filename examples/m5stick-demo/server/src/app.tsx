import { useState, useCallback } from "react";
import { useAgent } from "agents/react";

const DEVICE_ID = "m5stick-demo";

export default function App() {
  const [deviceConnected, setDeviceConnected] = useState(false);
  const [text, setText] = useState("");
  const [lastSent, setLastSent] = useState("");
  const [sending, setSending] = useState(false);

  const agent = useAgent({
    agent: "DeviceAgent",
    name: DEVICE_ID,
    query: { monitor: "1" },
    onMessage: useCallback((event: MessageEvent) => {
      try {
        const data = JSON.parse(String(event.data));
        if (data.type === "status") {
          setDeviceConnected(data.deviceConnected);
        } else if (data.type === "message") {
          setLastSent(data.text);
        }
      } catch {
        // Not JSON or protocol message
      }
    }, [])
  });

  const sendText = useCallback(async () => {
    if (!text.trim() || sending) return;
    setSending(true);
    try {
      await agent.call("sendMessage", [text]);
    } catch (err) {
      console.error("Failed to send text:", err);
    } finally {
      setSending(false);
    }
  }, [agent, text, sending]);

  return (
    <div className="min-h-screen bg-gray-50 p-8">
      <div className="max-w-2xl mx-auto space-y-6">
        <h1 className="text-2xl font-bold text-gray-900">
          M5Stick Text Sender
        </h1>

        <div className="flex items-center gap-2 text-sm text-gray-600">
          <span className="text-xl">{deviceConnected ? "\u2705" : "\u274C"}</span>
          <span className="font-mono">{DEVICE_ID}</span>
          <span>{deviceConnected ? "connected" : "waiting for device"}</span>
        </div>

        <div className="space-y-2">
          <label htmlFor="text-input" className="block text-sm font-medium text-gray-700">
            Text to display
          </label>
          <textarea
            id="text-input"
            value={text}
            onChange={(e) => setText(e.target.value)}
            placeholder="Type something to send to the M5Stick..."
            rows={6}
            className="w-full px-4 py-3 border border-gray-300 rounded-lg text-sm font-mono focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent resize-y"
          />
          <button
            type="button"
            onClick={sendText}
            disabled={!text.trim() || sending}
            className="px-4 py-2 bg-blue-600 text-white text-sm font-medium rounded-lg hover:bg-blue-700 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            {sending ? "Sending..." : "Send to Device"}
          </button>
        </div>

        {lastSent && (
          <div className="space-y-2">
            <h2 className="text-sm font-medium text-gray-700">Last Sent</h2>
            <pre className="p-4 bg-gray-900 text-green-400 text-sm font-mono rounded-lg overflow-auto max-h-64 whitespace-pre-wrap">{lastSent}</pre>
          </div>
        )}
      </div>
    </div>
  );
}
