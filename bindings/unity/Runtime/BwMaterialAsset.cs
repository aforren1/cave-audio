// BwMaterialAsset.cs — an acoustic material as a Project asset (Create > BwAudio > Acoustic Material).
// Reference it from BwAcousticGeometry. Either a named engine preset, or custom 3-band coefficients.
// Bands are low / mid / high; each value 0..1. Resolved into an engine material token at load time.
using System;
using UnityEngine;

namespace CaveAudio
{
    [CreateAssetMenu(menuName = "BwAudio/Acoustic Material", fileName = "AcousticMaterial")]
    public sealed class BwMaterialAsset : ScriptableObject
    {
        public enum Source { Preset, Custom }
        public Source source = Source.Preset;

        [Tooltip("Engine preset name (case-insensitive): generic, brick, concrete, ceramic, gravel, " +
                 "carpet, glass, plaster, wood, metal, rock.")]
        public string preset = "concrete";

        [Header("Custom coefficients (x=low, y=mid, z=high band; 0..1)")]
        [Tooltip("Fraction absorbed on reflection (1 = dead, 0 = perfect mirror), per band.")]
        public Vector3 absorption = new Vector3(0.1f, 0.1f, 0.1f);
        [Range(0f, 1f), Tooltip("Diffuse-vs-specular reflection (surface roughness).")]
        public float scattering = 0.5f;
        [Tooltip("Fraction passing THROUGH the surface (the spectral tilt of occluded sound), per band.")]
        public Vector3 transmission = new Vector3(0.05f, 0.05f, 0.05f);

        /// <summary>Mint this material into the engine; returns its token. Call at load time only.</summary>
        public uint Resolve(IntPtr engine)
        {
            if (source == Source.Preset)
                return Bw.bw_material_preset(engine, preset);
            return Bw.bw_material_define(engine,
                new[] { absorption.x, absorption.y, absorption.z }, scattering,
                new[] { transmission.x, transmission.y, transmission.z });
        }
    }
}
