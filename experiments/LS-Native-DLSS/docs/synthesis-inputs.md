# What the final synthesis consumes

This is an independently derived static description of resource 256 from the
provided native DLL, not copied LS source, a replacement shader or a GPU result.
The 3132-byte DXBC payload has the already recorded SHA256. For local inspection
it was translated by Wine vkd3d 1.2 to SPIR-V, validated with SPIR-V Tools and
inspected as assembly. The translator's output was not run on a GPU and its
pixel equivalence is not established. Cross-check using Microsoft's
D3DDisassemble before using any numeric convention in a replacement.

## Structure observed in the translated instruction stream

- t0/t1 supply source colours. Source ordering is still unknown.
- t2 and t3 are each four-channel displacement fields. For each field, xy moves
  the sampling position for t0; zw moves the sampling position for t1.
  These are **two motion hypotheses with two directions each**, rather than a
  single forward optical-flow field. Their physical producer and interpretation
  of occluded pixels still require runtime/controlled-image verification.
- Both displacement fields are multiplied by ResolutionInvScale (byte 24).
- The shader samples the fields at unwarped normalized pixel-center positions.
  The t0 sampling offset uses `2 * Timestamp`; t1 uses `2 * (1 - Timestamp)`.
  Sampling positions are normalized using the width/height queried from t0.
  Therefore the consumed fields act as scaled pixel displacements here. This
  does not establish that their original producer emits full-frame game motion,
  that Timestamp grows in displayed temporal order, or that NGX uses these units.
- Four colour samples result: t0 along each field's xy, t1 along each field's zw.
- t4 is **not another displacement input in this pass**. It supplies four
  scalar blending inputs, sampled at the corresponding warped positions:
  r for t0/field A, g for t1/field A, b for t0/field B, a for t1/field B.
- The shader exponentiates these scalar values, normalizes across all four,
  applies `(1 - Timestamp)` to the t0 colours and `Timestamp` to the t1 colours,
  then renormalizes the weighted colour sum (with a small epsilon).
  Calling the t4 values “confidence logits” is an interpretation of their
  mathematical role; their training, calibration and producer are unknown.

Texture formats, sampler filtering/addressing, exact time direction, colour
transfer functions, field resolution and signed displacement conventions must
still be checked at runtime. None is proven by the generic RDEF names Input1..5.

## Consequences for NVIDIA integration

Substituting one NVOF forward-vector texture into t2 is not a compatible
replacement: the native shader also expects the reverse direction, a second
hypothesis and four blending inputs. Filling them with duplicated vectors and
equal weights would remove learned visibility/ambiguity information and can
increase ghosting/disocclusion artifacts. Do not present that as a quality fix.

There are two separate research paths:

1. Full synthesis replacement by a completed DLSS generated image in the native
   output slot. NVOF becomes an input to our compatible NGX backend; the native
   intermediate-flow encoding need not be impersonated. LS keeps presentation.
   Exact source content IDs and GPU resource lifetimes must still be established.
2. NVIDIA-flow-assisted native LS synthesis, requiring a proven adapter for both
   directions, hypotheses and confidence inputs. It keeps the LS image generator,
   so it does not by itself fulfill replacement of both flow and generation.

The user's goal is path 1 with a standalone proxy. Path 2 is a comparison branch,
not the primary design. Initial shadow execution retains native LS analysis and
therefore costs additional GPU time; eventually avoiding those passes is a
separate dependency/lifetime change, not merely turning off one shader.

## Reproduce locally on Linux (no Windows/GPU needed)

Install Wine vkd3d-compiler and SPIR-V Tools separately. No third-party binaries
or proprietary shader outputs are distributed by this repository.

```sh
python tools/inspect_synthesis.py /path/to/Lossless_original.dll --output local/synthesis
```

Generated assembly/provenance remains local and ignored by git. The helper
requires the exact original DLL/shader identities and records that no native
execution or pixel-equivalence test took place.
