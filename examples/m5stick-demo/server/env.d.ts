/* eslint-disable */
declare namespace Cloudflare {
	interface GlobalProps {
		mainModule: typeof import("./src/server");
		durableNamespaces: "DeviceAgent";
	}
	interface Env {
		DeviceAgent: DurableObjectNamespace<import("./src/server").DeviceAgent>;
	}
}
interface Env extends Cloudflare.Env {}
