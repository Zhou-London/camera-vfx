# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.


from .base import Effect, requires_reload
from .image_encoding import ImageEncoding
from .true_hdr import TrueHDR
from .video_frame_generation import Mode, VideoFrameGeneration
from .video_super_res import QualityLevel, VideoSuperRes

VideoFrameGeneration.ImageEncoding = ImageEncoding
VideoFrameGeneration.Mode = Mode
VideoSuperRes.ImageEncoding = ImageEncoding
VideoSuperRes.QualityLevel = QualityLevel

__all__ = [
    "Effect",
    "ImageEncoding",
    "QualityLevel",
    "TrueHDR",
    "VideoFrameGeneration",
    "VideoSuperRes",
    "requires_reload",
]
