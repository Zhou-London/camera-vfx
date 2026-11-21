# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION is
# strictly prohibited.


from __future__ import annotations

from dataclasses import dataclass
from numbers import Integral
from typing import Any

from .base import Effect, check_dlpack_device
from .image_encoding import ImageEncoding

# VFX SDK parameter names (from nvVFXTrueHDR.h)
_PARAM_CONTRAST = "Contrast"
_PARAM_SATURATION = "Saturation"
_PARAM_MIDDLE_GRAY = "MiddleGray"
_PARAM_LUMINANCE = "Luminance"
_PARAM_DEBANDING_OFF = "DebandingOff"

# (min, max, default, sdk_name) — matches nvVFXTrueHDR.h 1.2.0.0.
_TRUEHDR_TUNABLES: dict[str, tuple[int, int, int, str]] = {
    "contrast": (0, 200, 100, _PARAM_CONTRAST),
    "saturation": (0, 200, 100, _PARAM_SATURATION),
    "middle_gray": (10, 100, 50, _PARAM_MIDDLE_GRAY),
    "luminance": (400, 2000, 650, _PARAM_LUMINANCE),
    "debanding_off": (0, 1, 0, _PARAM_DEBANDING_OFF),
}


@dataclass
class TrueHDROutput:
    image: Any  # DLPack capsule: (H, W) uint32 packed R10G10B10A2


class TrueHDR(Effect):
    """
    AI-powered SDR-to-HDR10 conversion effect.

    Framework-agnostic: accepts any DLPack-compatible CUDA array and returns
    a DLPack capsule that can be consumed by any framework.

    Same-resolution: output dimensions are pinned to the input dimensions on
    each call. Input dimensions are inferred from the input array; just call
    run() and load() will run lazily. Optionally call load() explicitly to
    prewarm the NGX feature ahead of the first run() (dimensions are not
    required at load time).

    Tunables (contrast, saturation, middle_gray, luminance, debanding_off)
    can be set at init or changed dynamically between calls; the SDK
    re-reads them per run().

    Args:
        contrast: Contrast tunable, [0..200] (default: 100)
        saturation: Saturation tunable, [0..200] (default: 100)
        middle_gray: Middle-grey reference, [10..100] (default: 50)
        luminance: Target HDR display peak in nits, [400..2000] (default: 650)
        debanding_off: Set to 1 to skip the DL-Debander pass (default: 0)
        device: CUDA device index (default: 0)

    Example with PyTorch:
        >>> import torch
        >>> from nvvfx import TrueHDR
        >>> input_frame = torch.rand(3, 540, 960, device="cuda")
        >>> with TrueHDR(luminance=1000) as thdr:
        ...     result = thdr.run(input_frame)
        ...     hdr10 = torch.from_dlpack(result.image).clone()
        >>> hdr10.shape, hdr10.dtype
        (torch.Size([540, 960]), torch.uint32)
    """

    _SELECTOR = "TrueHDR"

    def __init__(
        self,
        *,
        contrast: int = 100,
        saturation: int = 100,
        middle_gray: int = 50,
        luminance: int = 650,
        debanding_off: int = 0,
        device: int = 0,
    ) -> None:
        super().__init__(device)
        # TrueHDR always emits packed RGB10A2 output (input stays RGB8).
        # Set once here so per-call _ensure_dims() only manages buffer sizes.
        self._effect.set_output_encoding(int(ImageEncoding.RGB10A2))
        self._width: int | None = None
        self._height: int | None = None
        self.contrast = contrast
        self.saturation = saturation
        self.middle_gray = middle_gray
        self.luminance = luminance
        self.debanding_off = debanding_off

    def _set_tunable(self, py_name: str, value: int) -> None:
        lo, hi, _default, sdk_name = _TRUEHDR_TUNABLES[py_name]
        # Reject bool (a subclass of int) and non-integral numerics to avoid
        # silent truncation like int(99.9) -> 99 driving unintended SDK params.
        if isinstance(value, bool) or not isinstance(value, Integral):
            raise TypeError(f"{py_name} must be an integer, got {type(value).__name__}")
        ivalue = int(value)
        if not (lo <= ivalue <= hi):
            raise ValueError(f"{py_name}={value} out of range [{lo}..{hi}] for TrueHDR tunable")
        setattr(self, f"_{py_name}", ivalue)
        self._effect.set_u32(sdk_name, ivalue)

    @property
    def contrast(self) -> int:
        """Contrast tunable, [0..200]."""
        return self._contrast

    @contrast.setter
    def contrast(self, value: int) -> None:
        self._set_tunable("contrast", value)

    @property
    def saturation(self) -> int:
        """Saturation tunable, [0..200]."""
        return self._saturation

    @saturation.setter
    def saturation(self, value: int) -> None:
        self._set_tunable("saturation", value)

    @property
    def middle_gray(self) -> int:
        """Middle-grey reference, [10..100]."""
        return self._middle_gray

    @middle_gray.setter
    def middle_gray(self, value: int) -> None:
        self._set_tunable("middle_gray", value)

    @property
    def luminance(self) -> int:
        """Target HDR display peak luminance in nits, [400..2000]."""
        return self._luminance

    @luminance.setter
    def luminance(self, value: int) -> None:
        self._set_tunable("luminance", value)

    @property
    def debanding_off(self) -> int:
        """Skip DL-Debander pass, {0, 1}."""
        return self._debanding_off

    @debanding_off.setter
    def debanding_off(self, value: int) -> None:
        self._set_tunable("debanding_off", value)

    def _ensure_dims(self, width: int, height: int) -> None:
        # Re-attach only; SDK Load is dim-agnostic, so no reload needed.
        if self._width == width and self._height == height:
            return
        self._effect.set_input_image(width, height)
        self._effect.set_output_image(width, height)
        self._width = width
        self._height = height

    def run(
        self, input_array: Any, *, non_blocking: bool = False, stream_ptr: int = 0
    ) -> TrueHDROutput:
        """
        Convert an SDR frame to HDR10.

        Args:
            input_array: Input image array supporting DLPack protocol
                - Shape: (3, H, W) - RGB channels first
                - Dtype: float32
                - Device: CUDA
                - Range: [0, 1]
            non_blocking: If True, return immediately (async). Default False.
            stream_ptr: CUDA stream pointer as int (0 for default stream).
                For PyTorch: stream.cuda_stream
                For CuPy: stream.ptr

        Returns:
            TrueHDROutput containing an (H, W) uint32 DLPack tensor of
            packed R10G10B10A2 words. Convert with:

                - PyTorch: torch.from_dlpack(result.image).clone()
                - CuPy: cupy.from_dlpack(result.image).copy()

            IMPORTANT: The returned capsule references C++ memory. You must
            copy/clone it immediately before the next call or close().

        Raises:
            NvVFXError: If the SDK call fails.
            TypeError: If input doesn't support DLPack.
            ValueError: If input is not on CUDA device.
        """
        check_dlpack_device(input_array)

        shape = tuple(input_array.shape)
        if len(shape) != 3 or shape[0] != 3:
            raise ValueError(
                f"input_array must have shape (3, H, W) with channels-first RGB, got {shape}"
            )
        _, h, w = shape
        self._ensure_dims(w, h)

        if not self.is_loaded or self._needs_reload:
            super().load()

        self._effect.set_cuda_stream(stream_ptr)
        self._effect.transfer_input(input_array, stream_ptr)
        self._effect.run(non_blocking)

        return TrueHDROutput(image=self._effect.get_output(self.device))

    @property
    def input_width(self) -> int | None:
        """Input/output width in pixels."""
        return self._width

    @property
    def input_height(self) -> int | None:
        """Input/output height in pixels."""
        return self._height

    def __repr__(self) -> str:
        status = "closed" if self._closed else ("loaded" if self.is_loaded else "not loaded")
        dims = f"{self._width}x{self._height}" if self._width and self._height else "unset"
        return f"TrueHDR(dims={dims}, device={self.device}, status={status})"
