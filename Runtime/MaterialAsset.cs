// MaterialAsset.cs — an acoustic material as a Project asset (Create > Engine > Acoustic Material).
// Reference it from AcousticGeometry. Either a named engine preset, or custom 3-band coefficients.
// Bands are low / mid / high; each value 0..1. Resolved into an engine material token at load time.
using System;
using UnityEngine;

namespace BwAudio
{
    [CreateAssetMenu(menuName = "BwAudio/Acoustic Material", fileName = "AcousticMaterial")]
    public sealed class MaterialAsset : ScriptableObject
    {
        public enum Source { Preset, Custom }
        public Source source = Source.Preset;

        [Tooltip("One of the engine's built-in materials.")]
        public BwaMaterialPreset preset = BwaMaterialPreset.Concrete;

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
                return Bwa.bwa_material_preset(engine, preset);
            return Bwa.bwa_material_define(engine,
                new[] { absorption.x, absorption.y, absorption.z }, scattering,
                new[] { transmission.x, transmission.y, transmission.z });
        }
    }
}
