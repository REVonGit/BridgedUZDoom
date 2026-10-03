// halobridge.zs - native interface to the HaloDoom bridge.
// Lives in UZDoom's built-in package because mods can't declare natives.
// Positions are Doom map units, angles degrees, Doom conventions throughout;
// the native side converts to Halo's frame.

struct HaloBridge native
{
	native static bool IsActive();

	// flags, mapId, haloTick, playerPos, playerVel, groundZ, fovDeg
	native static int, int, int, Vector3, Vector3, double, double GetState();

	native static int ProxyCount();
	// id, flags, pos, radius, height, angle, kindHash, healthFrac
	native static int, int, Vector3, double, double, double, int, double GetProxy(int index);

	native static void PushMove(int tick, Vector3 delta);
	native static void PublishDoom(int tick, int flags, double angle, double pitch, double health, double armor);
	native static void PushDamage(int target, double amount, int dtypeHash, Vector3 origin, Vector3 dir, int flags);
	native static void PushDoomEvent(int type);

	// type (0 = none), amount, dtypeHash, source
	native static int, double, int, Vector3 PopHaloEvent();

	native static int RequestRay(Vector3 from, Vector3 to, bool objects);
	// reqId (0 = none), hit, entity, point, normal
	native static int, int, int, Vector3, Vector3 PopRayResult();

	native static int HashName(String s);
	native static void SaveCheckpoint();
	native static void LoadCheckpoint();
}
