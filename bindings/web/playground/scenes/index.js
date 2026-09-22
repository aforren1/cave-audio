/**
 * The scene table. Order is the picker's order.
 *
 * A scene is here only when it WORKS. The picker must never offer something that does nothing,
 * so a demo whose engine call this binding cannot reach yet stays out of this list rather than
 * shipping as an empty panel. bindings/web/README.md's "Playground" section names the one that
 * is missing and why.
 */
import { localization } from "./localization.js";
import { channelWalk } from "./channel_walk.js";
import { occlusion } from "./occlusion.js";
import { directivity } from "./directivity.js";
import { underwater } from "./underwater.js";
import { abx } from "./abx.js";

export const SCENES = [localization, channelWalk, occlusion, directivity, underwater, abx];
