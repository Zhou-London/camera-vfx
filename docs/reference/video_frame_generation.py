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

from .base import Effect, check_dlpack_device, requires_reload
from .image_encoding import ImageEncoding

_PARAM_AUTOMATIC_SHOT_CHANGE_DETECTION_ENABLED = "AutomaticShotChangeDetectionEnabled"
_PARAM_FRAME_INDEX = "FrameIndex"
_PARAM_FRAME_MULTIPLIER = "FrameMultiplier"
_PARAM_INPUT_HEIGHT = "InputHeight"
_PARAM_INPUT_WIDTH = "InputWidth"
_PARAM_MODE = "Mode"
_PARAM_SHOT_CHANGE = "ShotChange"
_PARAM_TIMESTEP = "Timestep"


@dataclass
class VideoFrameGenerationOutput:
    image: Any


class Mode(IntEnum):
    """Quality modes for Video Frame Generation."""

    LOW = 0
    MEDIUM = 1
    HIGH = 2


class VideoFrameGeneration(Effect):
    """
    AI-powered video frame generation effect.

    Generates intermediate frames from a previous/current frame pair. Set input
    dimensions and call load() before run(). Use run() for uniform frame-rate
    multiplication or run_at_timestep() for an explicit temporal position.

    Args:
        mode: Generation quality mode (default: MEDIUM)
        automatic_shot_change_detection_enabled: Enable automatic shot-change
            detection (default: True)
        device: CUDA device index (default: 0)
        image_encoding: Image encoding mode (default: RGB8). RGB8 accepts two
            `(3, H, W)` float32 CUDA inputs and returns `(3, H, W)` float32.
            RGB10A2 accepts two `(H, W)` uint32 CUDA inputs with packed
            R10G10B10A2 pixels and returns `(H, W)` uint32 packed pixels.
    """

    _SELECTOR = "VideoFrameGeneration"

    def __init__(
        self,
        mode: Mode = Mode.MEDIUM,
        automatic_shot_change_detection_enabled: bool = True,
        device: int = 0,
        image_encoding: ImageEncoding = ImageEncoding.RGB8,
    ) -> None:
        super().__init__(device)
        self._input_width: int | None = None
        self._input_height: int | None = None
        self._frame_multiplier = 2
        self._image_encoding = ImageEncoding.RGB8
        self.image_encoding = image_encoding
        self.mode = mode
        self.automatic_shot_change_detection_enabled = automatic_shot_change_detection_enabled

    @property
    def input_width(self) -> int | None:
        """Input frame width in pixels."""
        return self._input_width

    @input_width.setter
    @requires_reload
    def input_width(self, value: int) -> None:
        if value <= 0:
            raise ValueError("input_width must be positive")
        self._input_width = value

    @property
    def input_height(self) -> int | None:
        """Input frame height in pixels."""
        return self._input_height

    @input_height.setter
    @requires_reload
    def input_height(self, value: int) -> None:
        if value <= 0:
            raise ValueError("input_height must be positive")
        self._input_height = value

    @property
    def frame_multiplier(self) -> int:
        """Uniform frame-rate multiplier."""
        return self._frame_multiplier

    @frame_multiplier.setter
    def frame_multiplier(self, value: int) -> None:
        if value < 2:
            raise ValueError("frame_multiplier must be at least 2")
        self._frame_multiplier = value

    @property
    def mode(self) -> Mode:
        """Generation quality mode."""
        return self._mode

    @mode.setter
    @requires_reload
    def mode(self, value: Mode) -> None:
        self._mode = Mode(value)

    @property
    def automatic_shot_change_detection_enabled(self) -> bool:
        """Whether automatic shot-change detection is enabled."""
        return self._automatic_shot_change_detection_enabled

    @automatic_shot_change_detection_enabled.setter
    @requires_reload
    def automatic_shot_change_detection_enabled(self, value: bool) -> None:
        self._automatic_shot_change_detection_enabled = bool(value)

    @property
    def image_encoding(self) -> ImageEncoding:
        """Input and output image encoding mode."""
        return self._image_encoding

    @image_encoding.setter
    def image_encoding(self, value: ImageEncoding) -> None:
        new_value = ImageEncoding(value)
        old_value = getattr(self, "_image_encoding", None)
        if old_value == new_value:
            return

        was_loaded = self.is_loaded
        self._effect.set_image_encoding(int(new_value))
        self._image_encoding = new_value
        if was_loaded:
            self._needs_reload = True

    def load(self) -> None:
        """Load the effect for the configured input dimensions."""
        if self._input_width is None or self._input_height is None:
            raise ValueError("input_width and input_height must be set before load()")

        self._effect.set_input_image_pair(self._input_width, self._input_height)
        self._effect.set_u32(_PARAM_INPUT_WIDTH, self._input_width)
        self._effect.set_u32(_PARAM_INPUT_HEIGHT, self._input_height)
        self._effect.set_u32(_PARAM_MODE, int(self._mode))
        self._effect.set_u32(
            _PARAM_AUTOMATIC_SHOT_CHANGE_DETECTION_ENABLED,
            int(self._automatic_shot_change_detection_enabled),
        )
        super().load()
        self._effect.set_output_image(self._input_width, self._input_height)
        self._effect.set_u32(_PARAM_FRAME_MULTIPLIER, self._frame_multiplier)

    def run(
        self,
        previous_frame: Any,
        current_frame: Any,
        *,
        frame_index: int = 1,
        shot_change: bool = False,
        non_blocking: bool = False,
        stream_ptr: int = 0,
    ) -> VideoFrameGenerationOutput:
        """
        Generate a uniformly spaced intermediate frame.

        Args:
            previous_frame: Previous CUDA frame. RGB8 uses `(3, H, W)` float32;
                RGB10A2 uses `(H, W)` packed uint32.
            current_frame: Current CUDA frame with the same shape and encoding
                as `previous_frame`.
            frame_index: One-based intermediate frame index.
            shot_change: Mark this input pair as a shot change.
            non_blocking: If True, return immediately (async). Default False.
            stream_ptr: CUDA stream pointer as int (0 for default stream).
        """
        if frame_index < 1 or frame_index >= self._frame_multiplier:
            raise ValueError("frame_index must be in [1, frame_multiplier)")

        self._validate_run_inputs(previous_frame, current_frame)
        self._effect.set_u32(_PARAM_FRAME_MULTIPLIER, self._frame_multiplier)
        self._set_input_pair(previous_frame, current_frame, shot_change, stream_ptr)
        self._effect.set_u32(_PARAM_FRAME_INDEX, frame_index)
        self._effect.run(non_blocking)
        return VideoFrameGenerationOutput(image=self._effect.get_output(self.device))

    def run_at_timestep(
        self,
        previous_frame: Any,
        current_frame: Any,
        timestep: float,
        *,
        shot_change: bool = False,
        non_blocking: bool = False,
        stream_ptr: int = 0,
    ) -> VideoFrameGenerationOutput:
        """
        Generate an intermediate frame at an explicit temporal position.

        timestep must be in the open interval (0.0, 1.0), where 0.0 is the
        previous frame and 1.0 is the current frame.

        Inputs use `(3, H, W)` float32 for RGB8 or `(H, W)` packed uint32 for
        RGB10A2, matching the configured `image_encoding`.
        """
        if not 0.0 < timestep < 1.0:
            raise ValueError("timestep must be in the open interval (0.0, 1.0)")

        self._validate_run_inputs(previous_frame, current_frame)
        self._effect.set_u32(_PARAM_FRAME_MULTIPLIER, 0)
        self._set_input_pair(previous_frame, current_frame, shot_change, stream_ptr)
        self._effect.set_f32(_PARAM_TIMESTEP, timestep)
        self._effect.run(non_blocking)
        return VideoFrameGenerationOutput(image=self._effect.get_output(self.device))

    def _validate_run_inputs(self, previous_frame: Any, current_frame: Any) -> None:
        check_dlpack_device(previous_frame)
        check_dlpack_device(current_frame)
        if self.needs_reload:
            raise RuntimeError("load() must be called after changing effect configuration")
        if self._image_encoding == ImageEncoding.RGB10A2:
            if len(previous_frame.shape) != 2:
                raise ValueError("previous_frame must be a packed RGB10A2 tensor with shape (H, W)")
            if len(current_frame.shape) != 2:
                raise ValueError("current_frame must be a packed RGB10A2 tensor with shape (H, W)")
        else:
            if len(previous_frame.shape) != 3 or previous_frame.shape[0] != 3:
                raise ValueError(
                    "previous_frame must be a 3-channel CHW tensor with shape (3, H, W)"
                )
            if len(current_frame.shape) != 3 or current_frame.shape[0] != 3:
                raise ValueError(
                    "current_frame must be a 3-channel CHW tensor with shape (3, H, W)"
                )
        if previous_frame.shape != current_frame.shape:
            raise ValueError("previous_frame and current_frame must have matching shapes")
        if self._input_width is None or self._input_height is None:
            raise ValueError("input_width and input_height must be set before run()")

        if self._image_encoding == ImageEncoding.RGB10A2:
            height, width = previous_frame.shape
        else:
            _, height, width = previous_frame.shape
        if width != self._input_width or height != self._input_height:
            raise ValueError("Input frame dimensions must match input_width and input_height")
        if not self.is_loaded:
            raise RuntimeError("load() must be called before run()")

    def _set_input_pair(
        self, previous_frame: Any, current_frame: Any, shot_change: bool, stream_ptr: int
    ) -> None:
        self._effect.set_cuda_stream(stream_ptr)
        self._effect.transfer_input_pair(previous_frame, current_frame, stream_ptr)
        self._effect.set_u32(_PARAM_SHOT_CHANGE, int(shot_change))

    def __repr__(self) -> str:
        status = "closed" if self._closed else ("loaded" if self.is_loaded else "not loaded")
        input_dims = (
            f"{self._input_width}x{self._input_height}"
            if self._input_width and self._input_height
            else "not set"
        )
        return (
            f"VideoFrameGeneration("
            f"input={input_dims}, "
            f"mode={self._mode.name}, "
            f"frame_multiplier={self._frame_multiplier}, "
            f"image_encoding={self._image_encoding.name}, "
            f"device={self.device}, "
            f"status={status})"
        )
