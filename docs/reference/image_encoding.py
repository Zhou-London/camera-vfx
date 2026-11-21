# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.


from enum import IntEnum


class ImageEncoding(IntEnum):
    """
    Input and output image encoding for NVIDIA VFX effects.

    RGB8 uses normalized float32 RGB input and float32 RGB output.
    RGB10A2 uses packed uint32 R10G10B10A2 input and output.
    """

    RGB8 = 0
    RGB10A2 = 1
