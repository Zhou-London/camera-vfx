# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.


from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Any

from .base import Effect, check_dlpack_device
from .image_encoding import ImageEncoding

# VFX SDK parameter names
_PARAM_QUALITY = "QualityLevel"
_PARAM_STRENGTH = "Strength"
_PARAM_IMAGE_ENCODING_MODE = "ImageEncodingMode"


@dataclass
class VideoSuperResOutput:
    image: Any  # DLPack capsule


class QualityLevel(IntEnum):
    """
    Quality levels for Video Super Resolution.

    Selects the AI model and processing strategy. Modes are grouped by
    operation type; choose the group that matches your use case, then
    choose an intensity level within it (LOW → ULTRA trades speed for
    quality).

    Values 5-7 are reserved.

    **Standard upscaling modes (0-4):**
    Used when output resolution is larger than input. These models
    are trained on typical compressed video and will remove
    compression artifacts while upscaling.

    * **BICUBIC:** Non-AI bicubic interpolation (fastest, baseline quality)
    * **LOW:** AI upscaling, speed-optimized
    * **MEDIUM:** AI upscaling, balanced speed/quality
    * **HIGH:** AI upscaling, quality-favoring (default)
    * **ULTRA:** AI upscaling, maximum detail preservation

    **Denoise modes (8-11):**
    Same-resolution processing (output == input dimensions). Removes
    noise and compression artifacts (macro-blocking, mosquito noise)
    while preserving resolution. Higher intensities remove more noise
    but may soften fine texture.

    * **DENOISE_LOW:** Light cleanup, maximum texture preservation
    * **DENOISE_MEDIUM:** Moderate noise/artifact removal
    * **DENOISE_HIGH:** Aggressive noise/artifact removal
    * **DENOISE_ULTRA:** Maximum denoising strength

    **Deblur modes (12-15):**
    Same-resolution processing. Sharpens soft or blurry footage
    rather than targeting noise. Best for out-of-focus or
    motion-blurred sources.

    * **DEBLUR_LOW:** Light sharpening
    * **DEBLUR_MEDIUM:** Moderate sharpening
    * **DEBLUR_HIGH:** Aggressive sharpening
    * **DEBLUR_ULTRA:** Maximum sharpening strength

    **High-bitrate modes (16-19):**
    Upscaling optimized for high-bitrate or lossless sources (e.g.,
    ProRes, high-quality H.265) that have few compression artifacts.
    Unlike standard modes, these skip artifact suppression to avoid
    degrading already-clean detail.

    * **HIGHBITRATE_LOW:** AI upscaling, speed-optimized (clean source)
    * **HIGHBITRATE_MEDIUM:** AI upscaling, balanced (clean source)
    * **HIGHBITRATE_HIGH:** AI upscaling, quality-favoring (clean source)
    * **HIGHBITRATE_ULTRA:** AI upscaling, maximum detail (clean source)

    **Streaming modes (21, 23):**
    Upscaling optimized for broadcast and webcam streaming cases.

    * **STREAMING_MEDIUM:** Balanced streaming upscaling
    * **STREAMING_ULTRA:** Maximum-quality streaming upscaling
    """

    BICUBIC = 0
    LOW = 1
    MEDIUM = 2
    HIGH = 3
    ULTRA = 4
    # 5, 6, 7 reserved
    DENOISE_LOW = 8
    DENOISE_MEDIUM = 9
    DENOISE_HIGH = 10
    DENOISE_ULTRA = 11
    DEBLUR_LOW = 12
    DEBLUR_MEDIUM = 13
    DEBLUR_HIGH = 14
    DEBLUR_ULTRA = 15
    HIGHBITRATE_LOW = 16
    HIGHBITRATE_MEDIUM = 17
    HIGHBITRATE_HIGH = 18
    HIGHBITRATE_ULTRA = 19
    # 20, 22 reserved
    STREAMING_MEDIUM = 21
    STREAMING_ULTRA = 23


class VideoSuperRes(Effect):
    """
    AI-powered video super resolution effect.

    Framework-agnostic: accepts any DLPack-compatible CUDA array and returns
    a DLPack capsule that can be consumed by any framework.

    Set output dimensions via properties, then call load() before run().
    Input dimensions are inferred automatically from the input array.
    Quality level, strength, and dimensions can be changed dynamically between calls.

    Args:
        quality: Quality level for enhancement (default: HIGH)
        strength: Effect strength in [0.0, 1.0] (default: 1.0)
        device: CUDA device index (default: 0)
        image_encoding: Image encoding mode (default: RGB8). RGB8 accepts
            `(3, H, W)` float32 CUDA input in range `[0, 1]` and returns
            `(3, output_height, output_width)` float32. RGB10A2 accepts
            `(H, W)` uint32 CUDA input with packed R10G10B10A2 pixels and
            returns `(output_height, output_width)` uint32 packed pixels.

    Example with PyTorch:
        >>> import torch
        >>> from nvvfx import VideoSuperRes
        >>> input_frame = torch.rand(3, 540, 960, device="cuda")
        >>> with VideoSuperRes() as sr:
        ...     sr.output_width = 1920
        ...     sr.output_height = 1080
        ...     sr.load()
        ...     output = torch.from_dlpack(sr.run(input_frame).image).clone()
        >>> output.shape
        torch.Size([3, 1080, 1920])
    """

    _SELECTOR = "VideoSuperRes"

    def __init__(
        self,
        quality: QualityLevel = QualityLevel.HIGH,
        strength: float = 1.0,
        device: int = 0,
        image_encoding: ImageEncoding = ImageEncoding.RGB8,
    ) -> None:
        super().__init__(device)
        self._output_width: int | None = None
        self._output_height: int | None = None
        self._image_encoding = ImageEncoding.RGB8
        self.image_encoding = image_encoding
        self.quality = quality
        self.strength = strength

    @property
    def output_width(self) -> int | None:
        """Output width in pixels."""
        return self._output_width

    @output_width.setter
    def output_width(self, value: int) -> None:
        self._output_width = value
        if self._output_height is not None:
            self._effect.set_output_image(self._output_width, self._output_height)

    @property
    def output_height(self) -> int | None:
        """Output height in pixels."""
        return self._output_height

    @output_height.setter
    def output_height(self, value: int) -> None:
        self._output_height = value
        if self._output_width is not None:
            self._effect.set_output_image(self._output_width, self._output_height)

    @property
    def quality(self) -> QualityLevel:
        """Quality level for enhancement."""
        return self._quality

    @quality.setter
    def quality(self, value: QualityLevel) -> None:
        self._quality = QualityLevel(value)
        self._effect.set_u32(_PARAM_QUALITY, int(self._quality))

    @property
    def strength(self) -> float:
        """Strength in [0.0, 1.0]."""
        return self._strength

    @strength.setter
    def strength(self, value: float) -> None:
        if not 0.0 <= value <= 1.0:
            raise ValueError(f"strength must be in [0.0, 1.0], got {value}")
        self._strength = float(value)
        self._effect.set_f32(_PARAM_STRENGTH, self._strength)

    @property
    def image_encoding(self) -> ImageEncoding:
        """Input and output image encoding mode."""
        return self._image_encoding

    @image_encoding.setter
    def image_encoding(self, value: ImageEncoding) -> None:
        new_value = ImageEncoding(value)
        old_value = getattr(self, "_image_encoding", None)
        was_loaded = self.is_loaded

        self._image_encoding = new_value
        self._effect.set_u32(_PARAM_IMAGE_ENCODING_MODE, int(new_value))
        self._effect.set_image_encoding(int(new_value))

        if self._output_width is not None and self._output_height is not None:
            self._effect.set_output_image(self._output_width, self._output_height)

        if old_value is not None and old_value != new_value and was_loaded:
            self._needs_reload = True

    def run(
        self, input_array: Any, *, non_blocking: bool = False, stream_ptr: int = 0
    ) -> VideoSuperResOutput:
        """
        Apply super resolution to input array.

        Args:
            input_array: Input image array supporting DLPack protocol
                - RGB8 shape: (3, H, W), RGB channels first
                - RGB8 dtype/range: float32 in [0, 1]
                - RGB10A2 shape: (H, W)
                - RGB10A2 dtype/range: uint32 packed R10G10B10A2
                - Device: CUDA
            non_blocking: If True, return immediately (async). Default False.
            stream_ptr: CUDA stream pointer as int (0 for default stream).
                For PyTorch: stream.cuda_stream
                For CuPy: stream.ptr

        Returns:
            VideoSuperResOutput containing the upscaled image. Convert with:
                - PyTorch: torch.from_dlpack(result.image).clone()
                - CuPy: cupy.from_dlpack(result.image).copy()
                - JAX: jax.dlpack.from_dlpack(result.image)

            IMPORTANT: The returned capsule references C++ memory. You must
            copy/clone it immediately before the next call or close().

        Raises:
            NvVFXError: If load() was not called, output dimensions not set,
                or inference fails.
            TypeError: If input doesn't support DLPack.
            ValueError: If input is not on CUDA device.
        """
        check_dlpack_device(input_array)

        if self.needs_reload:
            raise RuntimeError(
                "VideoSuperRes configuration changed after load(); call load() before run()."
            )

        if self._image_encoding == ImageEncoding.RGB10A2:
            if len(input_array.shape) != 2:
                raise ValueError("RGB10A2 input must have shape (H, W)")
            h, w = input_array.shape
        else:
            if len(input_array.shape) != 3 or input_array.shape[0] != 3:
                raise ValueError("RGB8 input must have shape (3, H, W)")
            _, h, w = input_array.shape
        self._effect.set_input_image(w, h)

        self._effect.set_cuda_stream(stream_ptr)
        self._effect.transfer_input(input_array, stream_ptr)
        self._effect.run(non_blocking)

        output = VideoSuperResOutput(image=self._effect.get_output(self.device))
        return output

    def __repr__(self) -> str:
        status = "closed" if self._closed else ("loaded" if self.is_loaded else "not loaded")
        out_dims = (
            f"{self._output_width}x{self._output_height}"
            if self._output_width and self._output_height
            else "not set"
        )
        return (
            f"VideoSuperRes("
            f"output={out_dims}, "
            f"quality={self._quality.name}, "
            f"strength={self._strength}, "
            f"image_encoding={self._image_encoding.name}, "
            f"device={self.device}, "
            f"status={status})"
        )
