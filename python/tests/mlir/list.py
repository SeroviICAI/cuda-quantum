# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

# RUN: PYTHONPATH=../../ pytest -rP  %s | FileCheck %s

import os

import pytest

import cudaq


def test_make_kernel_list():
    """
    Test `cudaq.make_kernel` with a list of floats as parameters.
    """
    kernel, parameter = cudaq.make_kernel(list)
    # Kernel should only have 1 argument and parameter.
    got_arguments = kernel.arguments
    got_argument_count = kernel.argument_count
    assert len(got_arguments) == 1
    assert got_argument_count == 1
    # Dump the MLIR for FileCheck.
    print(kernel)


# CHECK-LABEL:   func.func @__nvqpp__mlirgen____nvqppBuilderKernel_{{.*}}(
# CHECK-SAME:    %[[VAL_0:.*]]: !cc.stdvec<f64>) attributes {"cudaq-entrypoint"
# CHECK:           return
# CHECK:         }

# leave for gdb debugging
if __name__ == "__main__":
    loc = os.path.abspath(__file__)
    pytest.main([loc, "-rP"])
