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

from collections.abc import Callable
from functools import wraps
from typing import Any, TypeVar

from .._ext import _Effect

F = TypeVar("F", bound=Callable[..., Any])

# DLPack device type constants
DLPACK_DEVICE_CUDA = 2
DLPACK_DEVICE_CUDA_MANAGED = 13


def check_dlpack_device(array: Any) -> None:
    """Validate array is on CUDA device via DLPack protocol."""
    if not hasattr(array, "__dlpack_device__"):
        raise TypeError("Input must support DLPack protocol (__dlpack_device__ method)")

    device_type, _ = array.__dlpack_device__()
    if device_type not in (DLPACK_DEVICE_CUDA, DLPACK_DEVICE_CUDA_MANAGED):
        raise ValueError(f"Input must be on CUDA device, got device type {device_type}")


def requires_reload(func: F) -> F:
    """
    Decorator for property setters that require reload after modification.

    When applied to a property setter, marks the effect as needing reload
    if the property is changed after the effect has been loaded.

    Example:
        class MyEffect(Effect):
            @property
            def some_param(self) -> int:
                return self._some_param

            @some_param.setter
            @requires_reload
            def some_param(self, value: int) -> None:
                self._some_param = value
    """

    @wraps(func)
    def wrapper(self: Any, value: Any) -> None:
        func(self, value)
        if getattr(self, "is_loaded", False):
            self._needs_reload = True

    return wrapper  # type: ignore[return-value]


class Effect:
    """
    Base class for all NVVFX effects.

    Provides a thin wrapper around the C++ _Effect bindings with state tracking
    and context manager support. Effect-specific classes inherit from this and
    implement load() and run().

    Attributes:
        is_loaded: Whether the effect has been loaded.
        needs_reload: Whether load() must be called again due to config changes.
        device: CUDA device index.
        selector: Effect selector string.
    """

    _SELECTOR: str = ""

    def __init__(self, device: int = 0) -> None:
        if not self._SELECTOR:
            raise ValueError("Effect subclass must define _SELECTOR")
        self._closed = True
        self._needs_reload = False
        self._effect = _Effect(self._SELECTOR, device)
        self._closed = False

    @property
    def is_loaded(self) -> bool:
        """Check if the effect is loaded."""
        return self._effect.is_loaded

    @property
    def needs_reload(self) -> bool:
        return self._needs_reload

    @property
    def device(self) -> int:
        """CUDA device index."""
        return self._effect.device

    @property
    def selector(self) -> str:
        """Effect selector string."""
        return self._effect.selector

    def load(self) -> None:
        """
        Load the effect.

        Must be called after setting all configuration parameters and before
        calling run(). Some parameters can be changed after load without
        requiring a reload.

        Raises:
            NvVFXError: If loading fails.
        """
        self._effect.load()
        self._needs_reload = False

    def run(self, input_array: Any, **kwargs: Any) -> Any:
        """
        Run inference. Subclasses must implement.

        Raises:
            NotImplementedError: If not implemented by subclass.
        """
        raise NotImplementedError("Subclass must implement run()")

    def close(self) -> None:
        """Release resources. Effect should not be used after this."""
        if not self._closed:
            self._effect.destroy()
            self._closed = True

    def __enter__(self) -> Effect:
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        self.close()
        return False

    def __del__(self) -> None:
        self.close()

    def __repr__(self) -> str:
        status = "loaded" if self.is_loaded else "not loaded"
        return f"<{self.__class__.__name__} device={self.device} {status}>"
