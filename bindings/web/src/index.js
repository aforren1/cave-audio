/**
 * index.js - the one import a page needs.
 *
 *   import { BwaEngine, Profile, SinkType } from "./dist/index.js";
 *
 * The raw layer and the idiomatic layer are re-exported too, but a PAGE cannot usefully hold
 * either: both run on the control thread and take the engine pointer, which exists only there.
 * They are here so a test, a node script or the in-page topology can reach them by name.
 */
export { BwaEngine, ClientSource, ClientPushSource, ClientListener } from "./client.js";
export {
  Profile, SinkType, SinkFlags, BwaError, Engine,
  MAX_CHANNELS, DEFAULT_GRID, CHANNEL_AUTO, GROUPS, EXTRA_LIS,
} from "./engine.js";
export { makeRaw, ABI, ABI_VERSION, CONSTANTS } from "./raw.js";
export { Host } from "./host.js";
export { OPS, slabBytes } from "./protocol.js";
