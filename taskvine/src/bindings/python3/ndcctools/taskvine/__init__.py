# Copyright (C) 2022- The University of Notre Dame
# This software is distributed under the GNU General Public License.
# See the file COPYING for details.

##
# @namespace ndcctools.taskvine
#
# TaskVine data intensive workflow framework - Python Interface
#
# TaskVine is a framework for building large scale distributed data intensive
# applications that run on clusters, clouds, grids, and similar distributed systems.
# A TaskVine application consists of a main program that creates a Manager object,
# and then submits Task objects that use File objects representing data sources.
# The manager distributes tasks across available workers and returns results to
# the main application.
#
# See the <a href=http://cctools.readthedocs.io/en/latest/taskvine>TaskVine Manual</a> for complete documentation.
#
# Recommended import statement:
#
# @code
# import ndcctools.taskvine as vine
# @endcode
#
# Relevant classes using recommended import statement:
#
# - @ref ndcctools.taskvine.manager.Manager "vine.Manager"
# - @ref ndcctools.taskvine.task.Task "vine.Task"
# - @ref ndcctools.taskvine.task.PythonTask "vine.PythonTask"
#
# Severless execution:
#
# - @ref ndcctools.taskvine.task.LibraryTask "vine.LibraryTask"
# - @ref ndcctools.taskvine.task.FunctionCall "vine.FunctionCall"
#
# Dask execution:
#
# - @ref ndcctools.taskvine.dask_executor.DaskVine "vine.DaskVine"
#

import warnings

from .manager import (
    Manager,
    Factory,
)
from .futures import (
    FuturesExecutor,
    FuturePythonTask,
    FutureFunctionCall,
    VineFuture,
)
from .file import File
from .task import (
    Task,
    PythonTask,
    PythonTaskNoResult,
    LibraryTask,
    FunctionCall,
)
from . import cvine


class DaskVineWarning(UserWarning):
    pass


try:
    import dask
    from packaging.version import Version
    vd = Version(dask.__version__)
    vr = Version("2024.12.0")
    if vd < vr:
        warnings.warn("ndcctools.taskvine.DaskVine only works with dask version >= 2024.12.0", DaskVineWarning)

    from .dask_executor import DaskVine
    from .dask_dag import DaskVineDag
except ImportError as e:
    warnings.warn(f"DaskVine not available. Couldn't find module: {e.name}", DaskVineWarning)

    ##
    # DaskVine compatibility class.
    # See @ref dask_executor.DaskVine
    class DaskVine:
        exception = ImportError()

        def __init__(*args, **kwargs):
            raise DaskVine.exception

    DaskVine.exception = e

__all__ = [
    "Manager",
    "File",
    "Factory",
    "LibraryTask",
    "FunctionCall",
    "FuturesExecutor",
    "FuturePythonTask",
    "FutureFunctionCall",
    "VineFuture",
    "Task",
    "PythonTask",
    "PythonTaskNoResult",
    "DaskVine",
    "DaskVineDag",
]

__version__ = cvine.vine_version_string()

# vim: set sts=4 sw=4 ts=4 expandtab ft=python:
