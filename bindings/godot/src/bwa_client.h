/* BwaEngineClient — the detach contract for every node that keeps a BwaEngine back-pointer.
 *
 * The engine node can be freed while its clients live elsewhere in the tree; without this,
 * their `owner` pointers dangle and the next setter (or the next _process tick) is a heap
 * use-after-free. Sources have their own registry (BwaEngine pulls them every frame); this
 * covers everything else that only needs the teardown notification: beds, speaker views,
 * dynamic geometry. A plain mixin, not a Godot class — it carries no reflection, only the
 * one virtual the engine calls on ITS _exit_tree.
 *
 * The protocol, mirroring BwaSource: register with the engine once the back-pointer is
 * taken, deregister when the CLIENT exits the tree first (so the engine's list never holds
 * a dangling child either), and on engine_gone() forget everything without calling back
 * into the engine — it is mid-teardown.
 */
#pragma once

namespace godot {

class BwaEngineClient {
public:
	virtual ~BwaEngineClient() = default;
	virtual void engine_gone() = 0;
};

} // namespace godot
