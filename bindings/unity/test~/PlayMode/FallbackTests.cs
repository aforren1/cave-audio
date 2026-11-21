// FallbackTests.cs: the desk fallback, a rig scene (profile Cave, a named device) opened on a
// machine without that device runs a headphone profile instead, when the scene opted in.
//
// Hermetic on purpose. The primary that is "missing" names a device no machine has, and every
// fallback lands on the offline sink, so nothing here depends on the audio hardware of the box that
// runs it. What the native engine was actually created with is read from bwa_get_audio_backend,
// which names the decode: "null" for Cave, "null (... sim)" for CaveSim, "null (... direct)" for
// Binaural. Engine.ActiveProfile alone would only repeat what the binding decided.
using System;
using System.Collections;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class FallbackTests
    {
        const string NoDevice = "bwa_test_no_such_device";
        const string NoLayout = "bwa_test_no_such_layout.json";
        Engine _eng;

        GameObject _extra;

        // The Engine's three internal seams. A release build is neither the Editor nor a debug
        // build. A development player is a debug build outside the Editor.
        static FieldInfo Seam(string name)
            => typeof(Engine).GetField(name, BindingFlags.NonPublic | BindingFlags.Static);
        static readonly FieldInfo EditorCheck = Seam("EditorCheck"), DebugBuildCheck = Seam("DebugBuildCheck"),
                                  PresentCheck = Seam("PresentCheck");
        static void Build(bool editor, bool debug)
        {
            EditorCheck.SetValue(null, (Func<bool>)(() => editor));
            DebugBuildCheck.SetValue(null, (Func<bool>)(() => debug));
        }

        [OneTimeSetUp] public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            Assert.IsNotNull(EditorCheck, "Engine.EditorCheck is gone or renamed");
            Assert.IsNotNull(DebugBuildCheck, "Engine.DebugBuildCheck is gone or renamed");
            Assert.IsNotNull(PresentCheck, "Engine.PresentCheck is gone or renamed");
        }

        [TearDown]
        public void TearDown()
        {
            EditorCheck?.SetValue(null, null);
            DebugBuildCheck?.SetValue(null, null);
            PresentCheck?.SetValue(null, null);
            Engine.OwnsAudioPredicate = null;
            LogAssert.ignoreFailingMessages = false;
            Harness.DestroyEngine(_eng); _eng = null;
            if (_extra) UnityEngine.Object.DestroyImmediate(_extra);
        }

        // The rig scene: the array on a named ASIO device that is not here.
        static void RigPrimary(Engine e)
        {
            e.profile = BwaProfile.Cave;
            e.sink = BwaSinkType.Asio;
            e.device = NoDevice;
        }

        static string Backend(Engine e) => Bwa.Backend(e.Handle) ?? "";

        [UnityTest]
        public IEnumerator MissingNamedDeviceTakesTheFallback()
        {
            LogAssert.Expect(LogType.Warning, new Regex("DESK FALLBACK.*Cave.*Asio.*" + NoDevice + ".*CaveSim.*Null"));
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            Assert.IsFalse(_eng.UsingFallback, "the fallback was reported before Start decided");
            Assert.AreEqual(BwaProfile.Cave, _eng.ActiveProfile);
            yield return Harness.Started(_eng);

            Assert.IsTrue(_eng.Ready, "the fallback did not start");
            Assert.IsTrue(_eng.UsingFallback);
            Assert.AreEqual(BwaProfile.CaveSim, _eng.ActiveProfile);
            Assert.AreEqual(BwaProfile.Cave, _eng.profile, "the rig setting was edited");
            Assert.AreEqual(BwaSinkType.Asio, _eng.sink, "the rig setting was edited");
            Assert.AreEqual(BwaSinkType.Null, Bwa.bwa_get_sink_type(_eng.Handle));
            StringAssert.StartsWith("null (", Backend(_eng));
            StringAssert.EndsWith("sim)", Backend(_eng), "the native engine is not on CaveSim");
        }

        // Auto with a named device that no backend lists is today's silent case: the offline sink,
        // still on Cave. With the fallback it is Binaural, which also has to get Binaural's layout
        // rule: a missing layout file is the default grid there, and a refused start on Cave.
        [UnityTest]
        public IEnumerator AutoWithAMissingNamedDeviceFallsBackAndBinauralToleratesNoLayout()
        {
            LogAssert.Expect(LogType.Warning, new Regex("DESK FALLBACK.*Auto.*" + NoDevice + ".*Binaural"));
            _eng = Harness.CreateEngine(e =>
            {
                e.profile = BwaProfile.Cave;
                e.sink = BwaSinkType.Auto;
                e.device = NoDevice;
                e.layoutFile = NoLayout;
                e.fallbackProfile = BwaFallbackProfile.Binaural;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);

            Assert.IsTrue(_eng.Ready, "a Binaural fallback refused to start over a missing layout file");
            Assert.IsTrue(_eng.UsingFallback);
            Assert.AreEqual(BwaProfile.Binaural, _eng.ActiveProfile);
            StringAssert.EndsWith("direct)", Backend(_eng), "the native engine is not on Binaural");
        }

        // The same file is missing for a CaveSim fallback, which pans over the layout and so must
        // refuse, as the array does.
        [UnityTest]
        public IEnumerator CaveSimFallbackStillNeedsTheLayout()
        {
            LogAssert.ignoreFailingMessages = true;               // the refused start logs errors on purpose
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.layoutFile = NoLayout;
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Decided);
            Assert.IsFalse(_eng.Ready, "CaveSim started over a layout file that does not exist");
            Assert.IsFalse(_eng.UsingFallback, "UsingFallback is true with no engine running");
            Assert.AreEqual(BwaProfile.Cave, _eng.ActiveProfile,
                            "ActiveProfile still names a fallback that did not start");
        }

        // Fallback Profile None is the feature off: a named backend with a missing device fails.
        [UnityTest]
        public IEnumerator NoFallbackProfileKeepsTheFailedStart()
        {
            LogAssert.ignoreFailingMessages = true;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e => { RigPrimary(e); e.fallbackSink = BwaSinkType.Null; });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.Decided);
                Assert.IsFalse(_eng.Ready, "a missing ASIO device started with no fallback set");
                Assert.IsFalse(_eng.UsingFallback);
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
                Assert.Greater(log.Count("bwa_start failed"), 0, "the failed start was not reported");
            }
        }

        // A release build with the toggle on behaves as today, whatever the fallback fields say.
        [UnityTest]
        public IEnumerator ReleaseBuildTakesNoFallback()
        {
            Build(editor: false, debug: false);
            LogAssert.ignoreFailingMessages = true;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    RigPrimary(e);
                    e.fallbackProfile = BwaFallbackProfile.CaveSim;
                    e.fallbackSink = BwaSinkType.Null;
                });
                Assert.IsTrue(_eng.fallbackOnlyInDevBuilds, "the toggle is not on by default");
                yield return Harness.Started(_eng);
                Assert.IsFalse(_eng.Ready, "a release build took the fallback with the toggle on");
                Assert.IsFalse(_eng.UsingFallback);
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
            }
        }

        // The toggle off is the opt-in for a release build.
        [UnityTest]
        public IEnumerator ReleaseBuildWithTheToggleOffTakesTheFallback()
        {
            Build(editor: false, debug: false);
            LogAssert.Expect(LogType.Error, new Regex("DESK FALLBACK"));   // an error outside the Editor
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
                e.fallbackOnlyInDevBuilds = false;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready);
            Assert.IsTrue(_eng.UsingFallback);
        }

        // A development player takes the fallback, and says so as an ERROR: outside the Editor
        // the machine may be the rig, where the same line means a typo in Device.
        [UnityTest]
        public IEnumerator DevelopmentPlayerLogsTheFallbackAsAnError()
        {
            Build(editor: false, debug: true);
            LogAssert.Expect(LogType.Error, new Regex("DESK FALLBACK.*" + NoDevice + ".*development build"));
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready);
            Assert.IsTrue(_eng.UsingFallback);
        }

        // The case the device lists cannot see: the primary is listed and does not open, so Auto
        // lands on the offline sink with the fallback untaken. No machine can stage a listed,
        // unopenable device, so the seam answers "present" for a device Auto will not find.
        [UnityTest]
        public IEnumerator ListedPrimaryThatDoesNotOpenReportsTheMissedFallback()
        {
            PresentCheck.SetValue(null, (Func<bool>)(() => true));
            LogAssert.Expect(LogType.Error, new Regex("NO AUDIO.*did NOT trigger.*" + NoDevice + ".*CaveSim"));
            _eng = Harness.CreateEngine(e =>
            {
                e.profile = BwaProfile.Cave;
                e.sink = BwaSinkType.Auto;
                e.device = NoDevice;
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "Auto did not land on the offline sink");
            Assert.AreEqual(BwaSinkType.Null, Bwa.bwa_get_sink_type(_eng.Handle));
            Assert.IsFalse(_eng.UsingFallback);
            Assert.AreEqual("null", Backend(_eng), "the native engine is not on Cave");
        }

        // The same silent Auto start with no fallback set is today's behavior and stays quiet.
        [UnityTest]
        public IEnumerator SilentAutoStartWithNoFallbackSetSaysNothingNew()
        {
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    e.profile = BwaProfile.Cave;
                    e.sink = BwaSinkType.Auto;
                    e.device = NoDevice;
                });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.Ready);
                Assert.AreEqual(BwaSinkType.Null, Bwa.bwa_get_sink_type(_eng.Handle));
                Assert.AreEqual(0, log.Count("did NOT trigger"));
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
            }
        }

        // The rig reads the head from Motive (Feed Listener off). On the fallback there is no
        // Motive: the connect is skipped, and the Listener Transform stands in when one is set.
        [UnityTest]
        public IEnumerator FallbackWithInternalTrackingFollowsTheListenerTransform()
        {
            _extra = new GameObject("desk head");
            _extra.transform.position = new Vector3(0.5f, 1.25f, -0.75f);
            LogAssert.Expect(LogType.Warning, new Regex("DESK FALLBACK: the primary device"));
            LogAssert.Expect(LogType.Warning, new Regex("tracker is not connected.*Listener Transform 'desk head'"));
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.feedListener = false;
                e.natnetServer = "127.0.0.1";
                e.listener = _extra.transform;
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.UsingFallback);
            Assert.AreEqual(BwaTrackerState.Disconnected, _eng.TrackerStatus, "the fallback connected the tracker");
            yield return ListenerReaches(_extra.transform);
            _extra.transform.position = new Vector3(-1f, 1.5f, 0.25f);
            yield return ListenerReaches(_extra.transform);
        }

        // With no Listener Transform the main camera is the head at a desk.
        [UnityTest]
        public IEnumerator FallbackWithInternalTrackingFollowsTheMainCamera()
        {
            Assert.IsNull(Camera.main, "the test scene already has a main camera");
            _extra = new GameObject("desk camera") { tag = "MainCamera" };
            _extra.AddComponent<Camera>();
            _extra.transform.position = new Vector3(0.25f, 1.6f, 0.5f);
            LogAssert.Expect(LogType.Warning, new Regex("DESK FALLBACK: the primary device"));
            LogAssert.Expect(LogType.Warning, new Regex("tracker is not connected.*main camera 'desk camera'"));
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.feedListener = false;
                e.listener = null;
                e.fallbackProfile = BwaFallbackProfile.Binaural;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.UsingFallback);
            Assert.AreEqual(BwaTrackerState.Disconnected, _eng.TrackerStatus);
            yield return ListenerReaches(_extra.transform);
        }

        // Without the fallback the same scene still connects the tracker (the socket opens, with
        // no Motive behind it), so the skip is the fallback's and not a change to the rig path.
        [UnityTest]
        public IEnumerator PrimaryWithInternalTrackingStillConnects()
        {
            _eng = Harness.CreateEngine(e =>
            {
                e.sink = BwaSinkType.Null;
                e.feedListener = false;
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Null;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready);
            Assert.IsFalse(_eng.UsingFallback);
            Assert.AreNotEqual(BwaTrackerState.Disconnected, _eng.TrackerStatus,
                               "the primary no longer connects the tracker");
        }

        // The engine's own listener position (room space) against the Transform's, within 1 mm.
        IEnumerator ListenerReaches(Transform head)
        {
            bool hit = false;
            yield return Harness.Until(() =>
            {
                Bwa.GetListenerPose(_eng.Handle, out var p, out _);
                return (new Vector3(p[0], p[1], p[2]) - Room.Pos(head.position)).magnitude < 1e-3f;
            }, 3.0, h => hit = h);
            Bwa.GetListenerPose(_eng.Handle, out var got, out _);
            Assert.IsTrue(hit, "the listener is at (" + got[0] + ", " + got[1] + ", " + got[2] +
                               "), not at " + Room.Pos(head.position) + ": nothing feeds it on the fallback");
        }

        // The fallback can end silent too: Auto, with a Fallback Device no backend lists, lands
        // on the offline sink. The warning promised a fallback, so the silence is an error.
        [UnityTest]
        public IEnumerator FallbackThatDoesNotOpenSaysItHasNoAudio()
        {
            LogAssert.Expect(LogType.Warning, new Regex("DESK FALLBACK: the primary device"));
            LogAssert.Expect(LogType.Error, new Regex("DESK FALLBACK has NO AUDIO either.*CaveSim.*Auto.*" + NoDevice + "_2"));
            _eng = Harness.CreateEngine(e =>
            {
                RigPrimary(e);
                e.fallbackProfile = BwaFallbackProfile.CaveSim;
                e.fallbackSink = BwaSinkType.Auto;
                e.fallbackDevice = NoDevice + "_2";
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "Auto did not land on the offline sink");
            Assert.IsTrue(_eng.UsingFallback);
            Assert.AreEqual(BwaSinkType.Null, Bwa.bwa_get_sink_type(_eng.Handle));
        }

        // A fallback that ASKED for the offline sink is silent on purpose: nothing to report.
        [UnityTest]
        public IEnumerator FallbackOnTheOfflineSinkOnPurposeIsNotAnError()
        {
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    RigPrimary(e);
                    e.fallbackProfile = BwaFallbackProfile.CaveSim;
                    e.fallbackSink = BwaSinkType.Null;
                    e.fallbackDevice = NoDevice + "_2";
                });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.UsingFallback);
                Assert.AreEqual(1, log.Count("DESK FALLBACK: the primary device"));
                Assert.AreEqual(0, log.Count("NO AUDIO"), "a fallback on the Null sink was reported as a failure");
            }
        }

        // A cluster node that does not own audio opens nothing: not the primary, not the fallback.
        [UnityTest]
        public IEnumerator NonOwnerTakesNoFallback()
        {
            Engine.OwnsAudioPredicate = () => false;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    RigPrimary(e);
                    e.fallbackProfile = BwaFallbackProfile.CaveSim;
                    e.fallbackSink = BwaSinkType.Null;
                });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.Decided);
                Assert.IsFalse(_eng.OwnsAudio);
                Assert.IsFalse(_eng.Ready, "a node that does not own audio opened the fallback");
                Assert.IsFalse(_eng.UsingFallback);
                Assert.AreEqual(BwaProfile.Cave, _eng.ActiveProfile);
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
                Assert.AreEqual("", log.Problems());
            }
        }

        // The offline sink opens no device, so it is always present: the primary runs.
        [UnityTest]
        public IEnumerator PresentPrimaryTakesNoFallback()
        {
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    e.profile = BwaProfile.Cave;
                    e.sink = BwaSinkType.Null;
                    e.fallbackProfile = BwaFallbackProfile.Binaural;
                    e.fallbackSink = BwaSinkType.Null;
                });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.Ready);
                Assert.IsFalse(_eng.UsingFallback, "a present primary took the fallback");
                Assert.AreEqual(BwaProfile.Cave, _eng.ActiveProfile);
                Assert.AreEqual("null", Backend(_eng), "the native engine is not on Cave");
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
            }
        }

        // A primary that is present and fails to start must fail: no second try on the fallback.
        // Here the primary is the offline sink (present) on Cave with a layout file that does not
        // exist, which bwa_start refuses. The Binaural fallback would have started over that same
        // missing file, so a retry would show as Ready.
        [UnityTest]
        public IEnumerator FailedStartOfAPresentPrimaryIsNotRetriedOnTheFallback()
        {
            LogAssert.ignoreFailingMessages = true;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e =>
                {
                    e.profile = BwaProfile.Cave;
                    e.sink = BwaSinkType.Null;
                    e.layoutFile = NoLayout;
                    e.fallbackProfile = BwaFallbackProfile.Binaural;
                    e.fallbackSink = BwaSinkType.Null;
                });
                yield return Harness.Started(_eng);
                Assert.IsTrue(_eng.Decided);
                Assert.IsFalse(_eng.Ready, "a failed start was retried on the fallback");
                Assert.IsFalse(_eng.UsingFallback);
                Assert.AreEqual(0, log.Count("DESK FALLBACK"));
                Assert.Greater(log.Count("bwa_start failed"), 0);
            }
        }

        // The presence rule itself, against whatever this machine lists. The fixed cases hold
        // everywhere; the per-device ones run for each device the box has and for none when it
        // has none.
        [Test]
        public void DevicePresentRule()
        {
            foreach (var p in new[] { BwaProfile.Cave, BwaProfile.Binaural, BwaProfile.CaveSim, BwaProfile.CaveBoth })
            {
                Assert.IsTrue(Engine.DevicePresent(p, BwaSinkType.Null, ""), "the offline sink needs no device");
                Assert.IsTrue(Engine.DevicePresent(p, BwaSinkType.Null, NoDevice));
                Assert.IsTrue(Engine.DevicePresent(p, BwaSinkType.Manual, null));
                Assert.IsFalse(Engine.DevicePresent(p, BwaSinkType.Auto, NoDevice), "Auto found a device nobody has");
            }

            var real = new[] { BwaSinkType.Asio, BwaSinkType.Wasapi, BwaSinkType.CoreAudio, BwaSinkType.Alsa,
                               BwaSinkType.AAudio, BwaSinkType.Jack, BwaSinkType.Worklet };
            bool anyArray = false;
            foreach (var b in real)
            {
                // ASIO, WASAPI and CoreAudio open a device from their list, so an unlisted name is
                // missing. The other four open strings their list never held (a JACK port pattern,
                // an ALSA PCM like hw:0,0, an AAudio id), so a named device there is present: the
                // rig's "hw:1,0" must never read as missing and send the rig to headphones.
                bool unlisted = b == BwaSinkType.Jack || b == BwaSinkType.Alsa ||
                                b == BwaSinkType.AAudio || b == BwaSinkType.Worklet;
                // ...but only where the engine library carries the backend. An ALSA scene on a
                // Windows desk is a missing device: that machine is not the rig, and bwa_start
                // would fail there with "this build has no alsa backend".
                // A Linux library carries JACK and ALSA only when CMake found their headers, so
                // there the answer is the build's, not the platform's: test_cave_both asks the
                // build itself, and this pins only what the platform settles.
                bool? carried = OnThisPlatform(b);
                if (carried.HasValue)
                {
                    foreach (var p in new[] { BwaProfile.Cave, BwaProfile.Binaural })
                        Assert.AreEqual(unlisted && carried.Value, Engine.DevicePresent(p, b, NoDevice),
                                        b + " with a device name its list does not hold");
                    if (unlisted) Assert.AreEqual(carried.Value, Engine.DevicePresent(BwaProfile.Cave, b, "hw:1,0"), b + " and hw:1,0");
                }
                uint n = Engine.DeviceCount(b);
                // A named backend with no device name: present exactly when it lists something.
                Assert.AreEqual(n > 0, Engine.DevicePresent(BwaProfile.Cave, b, ""), b + " with no device name");
                Assert.AreEqual(n > 0, Engine.DevicePresent(BwaProfile.Cave, b, null));
                // Auto offers the array every backend but the three stereo-only ones.
                bool stereoOnly = b == BwaSinkType.Wasapi || b == BwaSinkType.AAudio || b == BwaSinkType.Worklet;
                if (n > 0 && !stereoOnly) anyArray = true;
                for (uint i = 0; i < n; i++)
                {
                    string name = Engine.DeviceName(b, i), id = Engine.DeviceId(b, i);
                    if (string.IsNullOrEmpty(name)) continue;
                    Assert.IsTrue(Engine.DevicePresent(BwaProfile.Cave, b, name), b + " does not find its own '" + name + "'");
                    if (!string.IsNullOrEmpty(id))
                        Assert.IsTrue(Engine.DevicePresent(BwaProfile.Cave, b, id), b + " does not find its own id '" + id + "'");
                    Assert.IsTrue(Engine.DevicePresent(BwaProfile.Binaural, BwaSinkType.Auto, name),
                                  "Auto does not offer a stereo profile " + b + "'s '" + name + "'");
                    if (!stereoOnly)
                        Assert.IsTrue(Engine.DevicePresent(BwaProfile.Cave, BwaSinkType.Auto, name),
                                      "Auto does not offer the array " + b + "'s '" + name + "'");
                    // A stereo-only backend's device is not the array's, unless an array backend
                    // lists the same string too.
                    else if (!ListedByAnArrayBackend(name))
                        Assert.IsFalse(Engine.DevicePresent(BwaProfile.Cave, BwaSinkType.Auto, name),
                                       "Auto would hand the array to " + b + "'s '" + name + "'");
                    // The match is exact: a listed name with more text after it is another device.
                    if (!unlisted)
                        Assert.IsFalse(Engine.DevicePresent(BwaProfile.Cave, b, name + "_bwa_test_suffix"));
                }
            }
            Assert.AreEqual(anyArray, Engine.DevicePresent(BwaProfile.Cave, BwaSinkType.Auto, ""),
                            "Auto with no device name on the array");
            Assert.AreEqual(anyArray, Engine.DevicePresent(BwaProfile.CaveBoth, BwaSinkType.Auto, ""));
            Harness.Note("DevicePresentRule: asio=" + Engine.DeviceCount(BwaSinkType.Asio) +
                         " wasapi=" + Engine.DeviceCount(BwaSinkType.Wasapi) + " devices on this machine");
        }

        // The backends each platform's engine library carries, at the default build options.
        // Whether this platform's engine library carries the backend; null where the build decides.
        static bool? OnThisPlatform(BwaSinkType b)
        {
            switch (Application.platform)
            {
                case RuntimePlatform.WindowsEditor: case RuntimePlatform.WindowsPlayer:
                    return b == BwaSinkType.Asio || b == BwaSinkType.Wasapi;
                case RuntimePlatform.LinuxEditor: case RuntimePlatform.LinuxPlayer:
                    if (b == BwaSinkType.Jack || b == BwaSinkType.Alsa) return null;
                    return false;
                case RuntimePlatform.OSXEditor: case RuntimePlatform.OSXPlayer:
                    return b == BwaSinkType.CoreAudio;
                case RuntimePlatform.Android:
                    return b == BwaSinkType.AAudio;
                default: return false;
            }
        }

        static bool ListedByAnArrayBackend(string s)
        {
            foreach (var b in new[] { BwaSinkType.Asio, BwaSinkType.CoreAudio, BwaSinkType.Alsa, BwaSinkType.Jack })
                for (uint i = 0; i < Engine.DeviceCount(b); i++)
                    if (Engine.DeviceName(b, i) == s || Engine.DeviceId(b, i) == s) return true;
            return false;
        }

        // With Domain Reload off the test seam would leak into the next Play session.
        [Test]
        public void PlayEntryResetClearsTheSeams()
        {
            var reset = typeof(Engine).GetMethod("ResetOnPlay", BindingFlags.NonPublic | BindingFlags.Static);
            Assert.IsNotNull(reset, "Engine.ResetOnPlay is gone or renamed");
            Build(editor: false, debug: false);
            PresentCheck.SetValue(null, (Func<bool>)(() => true));
            reset.Invoke(null, null);
            foreach (var f in new[] { EditorCheck, DebugBuildCheck, PresentCheck })
                Assert.IsNull(f.GetValue(null), "the reset left " + f.Name);
        }
    }
}
