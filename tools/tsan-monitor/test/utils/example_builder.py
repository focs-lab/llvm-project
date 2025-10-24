"""Build example programs with TSan instrumentation."""

import subprocess
from pathlib import Path
from typing import List


class ExampleBuilder:
    """Builder for TSan-instrumented example programs."""

    def __init__(self, clang_path: str, monitor_path: str):
        """Initialize builder with paths to clang and monitor."""
        self.clang_path = clang_path
        self.monitor_path = monitor_path

    def build_example(self, source_file: Path, output_dir: Path) -> Path:
        """Build an example program with TSan instrumentation.

        Args:
            source_file: Path to the source C++ file
            output_dir: Directory where the executable should be built

        Returns:
            Path to the built executable

        Raises:
            RuntimeError: If compilation fails
        """
        example_name = source_file.stem
        exe_path = output_dir / example_name

        # Build compilation command
        cmd = [
            self.clang_path,
            "-fsanitize=thread",
            "-g",
            "-pthread",
            str(source_file),
            "-o", str(exe_path.absolute())
        ]

        # Run compilation
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=output_dir
        )

        if result.returncode != 0:
            error_msg = f"Build failed for {source_file.name}:\n"
            error_msg += f"Command: {' '.join(cmd)}\n"
            error_msg += f"Stderr: {result.stderr}\n"
            error_msg += f"Stdout: {result.stdout}"
            raise RuntimeError(error_msg)

        return exe_path

    def build_ir(self, source_file: Path, output_dir: Path) -> Path:
        """Generate LLVM IR for the example program.

        Args:
            source_file: Path to the source C++ file
            output_dir: Directory where the IR file should be generated

        Returns:
            Path to the generated IR file
        """
        example_name = source_file.stem
        ir_path = output_dir / f"{example_name}.ll"

        # Build IR generation command
        cmd = [
            self.clang_path,
            "-fsanitize=thread",
            "-g",
            "-S",
            "-emit-llvm",
            str(source_file),
            "-o", str(ir_path.absolute())
        ]

        # Run IR generation
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=output_dir
        )

        if result.returncode != 0:
            error_msg = f"IR generation failed for {source_file.name}:\n"
            error_msg += f"Command: {' '.join(cmd)}\n"
            error_msg += f"Stderr: {result.stderr}\n"
            error_msg += f"Stdout: {result.stdout}"
            raise RuntimeError(error_msg)

        return ir_path