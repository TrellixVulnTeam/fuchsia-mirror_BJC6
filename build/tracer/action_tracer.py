#!/usr/bin/env python3.8
"""Validates file system accesses of a subprocess command.

This uses a traced exection wrapper (fsatrace) to invoke a command,
captures a trace of file system {read,write} operations, and validates
those access against constraints such as declared inputs and outputs.
"""
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import dataclasses
import enum
import itertools
import os
import re
import shlex
import subprocess
import sys

from typing import AbstractSet, Any, Callable, Collection, FrozenSet, Iterable, Optional, Sequence, TextIO, Tuple

trailing_white_spaces = re.compile("\\\s+\r?\n")


def _partition(
        iterable: Iterable[Any],
        predicate: Callable[[Any],
                            bool]) -> Tuple[Sequence[Any], Sequence[Any]]:
    """Splits sequence into two sequences based on predicate function."""
    trues = []
    falses = []
    for item in iterable:
        if predicate(item):
            trues.append(item)
        else:
            falses.append(item)
    return trues, falses


class FileAccessType(enum.Enum):
    READ = enum.auto()
    WRITE = enum.auto()
    DELETE = enum.auto()


@dataclasses.dataclass
class MatchConditions(object):
    prefixes: FrozenSet[str] = dataclasses.field(default_factory=set)
    suffixes: FrozenSet[str] = dataclasses.field(default_factory=set)
    components: FrozenSet[str] = dataclasses.field(default_factory=set)

    def matches(self, path: str) -> bool:
        """Returns true if path matches any of the conditions."""
        if any(path.startswith(prefix) for prefix in self.prefixes):
            return True
        if any(path.endswith(prefix) for prefix in self.suffixes):
            return True
        if set(path.split(os.path.sep)).intersection(self.components):
            return True
        return False


def _find_first_index(
        iterable: Iterable[Any], pred: Callable[[Any], bool]) -> int:
    """Returns the index of the first element that satisfies the predicate, or -1."""
    for n, item in enumerate(iterable):
        if pred(item):
            return n
    return -1


@dataclasses.dataclass
class ToolCommand(object):
    """This models a command for invoking a tool in a shell.

    A command consists of the following components in order:
      * an optional list of environment variable overrides like 'HOME=/path'
      * one tool (script or binary in PATH)
      * an optional list of flags and arguments passed to the tool
    """
    tokens: Sequence[str] = dataclasses.field(default_factory=list)

    @property
    def _tool_index(self) -> int:
        # The first token that isn't 'X=Y' is the tool/script.
        return _find_first_index(self.tokens, lambda x: '=' not in x)

    @property
    def env_tokens(self):
        """Returns the environment overrides (X=Y) of a shell command."""
        return self.tokens[:self._tool_index]

    @property
    def tool(self):
        """Returns the tool/script of the command."""
        return self.tokens[self._tool_index]

    @property
    def args(self):
        """Returns all options and arguments after the tool of the command."""
        return self.tokens[self._tool_index + 1:]

    @property
    def _end_opts_index(self) -> int:
        # Find the position of '--', which is conventionally used to stop option
        # processing.
        return _find_first_index(self.tokens, lambda x: x == '--')

    def unwrap(self) -> "ToolCommand":
        # Assuming that '--' separates a wrapper from a command, this unwraps
        # a command one level.  (For example, this script is such a wrapper,
        # with the original command following '--' in trailing position.)
        end_opts_index = self._end_opts_index
        if end_opts_index == -1:
            # Deduce that the command is not wrapped.
            return self
        return ToolCommand(tokens=self.tokens[end_opts_index + 1:])


@dataclasses.dataclass
class FSAccess(object):
    """Represents a single file system access."""
    # One of: "read", "write" (covers touch), "delete" (covers move-from)
    op: FileAccessType
    # The path accessed
    path: str

    # TODO(fangism): for diagnostic purposes, we may want a copy of the fsatrace
    # line from which this access came.

    def __repr__(self):
        return f"({self.op} {self.path})"

    def should_check(
            self,
            ignore_conditions: MatchConditions,
            required_path_prefix: str = "") -> bool:
        """Predicate function use to filter out FSAccesses.

        Args:
          required_path_prefix: Accesses outside of this path prefix are not checked.
            An empty string means: check this access.
          ignore_conditions: prefixes, suffixes, components to ignore.

        Returns:
          true if this access should be checked.
        """
        if not self.path.startswith(required_path_prefix):
            return False

        return not ignore_conditions.matches(self.path)

    def allowed(
            self, allowed_reads: FrozenSet[str],
            allowed_writes: FrozenSet[str]) -> bool:
        """Validates a file system access against a set of allowed accesses.

        Args:
          allowed_reads: set of allowed read paths.
          allowed_writes: set of allowed write paths.

        Returns:
          True if this access is allowed.
        """
        if self.op == FileAccessType.READ:
            return self.path in allowed_reads
        elif self.op == FileAccessType.WRITE:
            return self.path in allowed_writes
        elif self.op == FileAccessType.DELETE:
            # TODO(fangism): separate out forbidded_deletes
            return self.path in allowed_writes
        raise ValueError(f"Unknown operation: {self.op}")


# Factory functions for making FSAccess objects.
def Read(path: str):
    return FSAccess(FileAccessType.READ, path)


def Write(path: str):
    return FSAccess(FileAccessType.WRITE, path)


def Delete(path: str):
    return FSAccess(FileAccessType.DELETE, path)


def _parse_fsatrace_line(fsatrace_line: str) -> Iterable[FSAccess]:
    """Parses an output line from fsatrace into a stream of FSAccesses.

    See: https://github.com/jacereda/fsatrace#output-format
    Moves are split into two operations: delete source, write destination

    Args:
      fsatrace_line: one line of trace from fsatrace

    Yields:
      0 to 2 FSAccess objects.
    """
    # ignore any lines that do not parse
    op, sep, path = fsatrace_line.partition("|")
    if sep != "|":
        return

    # op: operation code in [rwdtm]
    if op == "r":
        yield Read(path)
    elif op in {"w", "t"}:
        yield Write(path)
    elif op == "d":
        yield Delete(path)
    elif op == "m":
        # path: "destination|source"
        # The source is deleted, and the destination is written.
        dest, sep, source = path.partition("|")
        if sep != "|":
            raise ValueError("Malformed move line: " + fsatrace_line)
        yield Delete(source)
        yield Write(dest)


def parse_fsatrace_output(fsatrace_lines: Iterable[str]) -> Iterable[FSAccess]:
    """Returns a stream of FSAccess objects."""
    return itertools.chain.from_iterable(
        _parse_fsatrace_line(line) for line in fsatrace_lines)


def _abspaths(container: Iterable[str]) -> AbstractSet[str]:
    return {os.path.abspath(f) for f in container}


@dataclasses.dataclass
class AccessConstraints(object):
    """Set of file system accesses constraints."""
    allowed_reads: FrozenSet[str] = dataclasses.field(default_factory=set)
    allowed_writes: FrozenSet[str] = dataclasses.field(default_factory=set)
    required_writes: FrozenSet[str] = dataclasses.field(default_factory=set)
    # TODO(fangism): forbidden_deletes should probably include declared inputs

    @property
    def inputs(self):
        # allowed_reads includes allowed_writes (and required_writes), so consider
        # "inputs" as their set-difference.
        return self.allowed_reads - self.allowed_writes - self.required_writes

    def fresh_outputs(self) -> AbstractSet[str]:
        """Identify the outputs that should not be fresh for a failed action.

        Compare timestamps of existing inputs and outputs.
        No access trace is needed for this check.

        Returns:
          Subset of declared outputs (required_writes) that are fresher than
          the newest input (allowed_reads).
        """
        # Find declared outputs that exist.
        existing_outputs = {
            f for f in self.required_writes if os.path.exists(f)
        }
        existing_inputs = {f for f in self.inputs if os.path.exists(f)}

        if not existing_inputs:
            # Then all outputs are considered fresh.
            return existing_outputs

        newest_input = max(existing_inputs, key=realpath_ctime)
        input_timestamp = realpath_ctime(newest_input)

        fresh_outputs = {
            out for out in existing_outputs
            if realpath_ctime(out) > input_timestamp
        }
        return fresh_outputs


@dataclasses.dataclass
class DepEdges(object):
    ins: FrozenSet[str] = dataclasses.field(default_factory=set)
    outs: FrozenSet[str] = dataclasses.field(default_factory=set)

    def abspaths(self) -> "DepEdges":
        return DepEdges(ins=_abspaths(self.ins), outs=_abspaths(self.outs))


def parse_dep_edges(depfile_line: str) -> DepEdges:
    """Parse a single line of a depfile.

    This assumes that all depfile entries are formatted onto a single line.
    TODO(fangism): support more generalized forms of input, e.g. multi-line.
      See https://github.com/ninja-build/ninja/blob/master/src/depfile_parser_test.cc

    Args:
      depfile_line: has the form "OUTPUT1 [OUTPUT2 ...]: INPUT [INPUT ...]"

    Returns:
      A DepEdges object represending a dependency between inputs and outputs.

    Raises:
      ValueError if unable to parse dependency entry.
    """
    outs, sep, ins = depfile_line.strip().partition(":")
    if not sep:
        raise ValueError("Failed to parse depfile entry:\n" + depfile_line)
    return DepEdges(ins=set(shlex.split(ins)), outs=set(shlex.split(outs)))


@dataclasses.dataclass
class DepFile(object):
    """DepFile represents a collection of dependency edges."""
    deps: Collection[DepEdges] = dataclasses.field(default_factory=list)

    @property
    def all_ins(self) -> AbstractSet[str]:
        """Returns a set of all dependency inputs."""
        return {f for dep in self.deps for f in dep.ins}

    @property
    def all_outs(self) -> AbstractSet[str]:
        """Returns a set of all dependency outputs."""
        return {f for dep in self.deps for f in dep.outs}


def parse_depfile(depfile_lines: Iterable[str]) -> DepFile:
    """Parses a depfile into a set of inputs and outputs.

    See https://github.com/ninja-build/ninja/blob/master/src/depfile_parser_test.cc
    for examples of format using Ninja syntax.

    TODO(fangism): ignore blank/comment lines

    Args:
      depfile_lines: lines from a depfile

    Returns:
      DepFile object, collection of dependencies.
    """

    # Go through all lines and join continuations. Doing this manually to avoid
    # copies as much as possible.
    lines = []
    current_line = ""
    for line in depfile_lines:
        # We currently don't allow consecutive backslashes in filenames to
        # simplify depfile parsing. Support can be added if use cases come up.
        #
        # Ninja's implementation:
        # https://github.com/ninja-build/ninja/blob/5993141c0977f563de5e064fbbe617f9dc34bb8d/src/depfile_parser.cc#L39
        if r"\\" in line:
            raise ValueError(
                f'Consecutive backslashes found in depfile line "{line}", this is not supported by action tracer'
            )
        # We currently don't have any use cases with trailing whitespaces in
        # file names, so treat them as errors when they show up at the end of a
        # line, because users usually want a line continuation. We can
        # reconsider this check when use cases come up.
        if trailing_white_spaces.match(line):
            raise ValueError(
                f'Backslash followed by trailing whitespaces at end of line "{line}", remove whitespaces for proper line continuation'
            )

        if line.endswith(("\\\n", "\\\r\n")):
            current_line += line.rstrip("\\\r\n")
            continue
        current_line += line
        lines.append(current_line)
        current_line = ""

    if current_line:
        raise ValueError("Line continuation found at end of file")

    return DepFile(deps=[parse_dep_edges(line) for line in lines])


def abspaths_from_depfile(depfile: DepFile,
                          allowed_abspaths: FrozenSet[str]) -> Collection[str]:
    return [
        f for f in (depfile.all_ins | depfile.all_outs)
        if f not in allowed_abspaths and os.path.isabs(f)
    ]


@dataclasses.dataclass
class Action(object):
    """Represents a set of parameters of a single build action."""
    inputs: Sequence[str] = dataclasses.field(default_factory=list)
    outputs: Collection[str] = dataclasses.field(default_factory=list)
    depfile: Optional[str] = None
    parsed_depfile: Optional[DepFile] = None

    def access_constraints(
            self, writeable_depfile_inputs=False) -> AccessConstraints:
        """Build AccessConstraints from action attributes."""
        # Action is required to write outputs and depfile, if provided.
        required_writes = {path for path in self.outputs}

        # Paths that the action is allowed to write.
        # Actions may touch files other than their listed outputs.
        allowed_writes = required_writes.copy()

        allowed_reads = set(self.inputs)

        if self.depfile:
            # Writing the depfile is not required (yet), but allowed.
            allowed_writes.add(self.depfile)
            if os.path.exists(self.depfile):
                with open(self.depfile, "r") as f:
                    self.parsed_depfile = parse_depfile(f)

                if (writeable_depfile_inputs):
                    allowed_writes.update(self.parsed_depfile.all_ins)
                else:
                    allowed_reads.update(self.parsed_depfile.all_ins)
                allowed_writes.update(self.parsed_depfile.all_outs)

        # Everything writeable is readable.
        allowed_reads.update(allowed_writes)

        return AccessConstraints(
            # Follow links in all inputs because fsatrace will log access to link
            # destination instead of the link.
            allowed_reads=_abspaths(
                os.path.realpath(path) for path in allowed_reads),
            # TODO(fxbug.dev/69049): Should we follow links of outputs as well?
            # What's our stance on writing to soft links?
            allowed_writes=_abspaths(allowed_writes),
            required_writes=_abspaths(required_writes))


def _sorted_join(elements: Iterable[str], joiner: str):
    return joiner.join(sorted(elements))


@dataclasses.dataclass
class FSAccessSet(object):
    reads: FrozenSet[str] = dataclasses.field(default_factory=set)
    writes: FrozenSet[str] = dataclasses.field(default_factory=set)
    deletes: FrozenSet[str] = dataclasses.field(default_factory=set)

    @property
    def all_accesses(self):
        return self.reads | self.writes | self.deletes

    def __str__(self):
        if not self.all_accesses:
            return "[empty accesses]"
        text = ""
        if self.reads:
            text += "\nReads:\n  " + _sorted_join(self.reads, "\n  ")
        if self.writes:
            text += "\nWrites:\n  " + _sorted_join(self.writes, "\n  ")
        if self.deletes:
            text += "\nDeletes:\n  " + _sorted_join(self.deletes, "\n  ")
        # trim first newline if there is one
        return text.lstrip("\n")


def finalize_filesystem_accesses(accesses: Iterable[FSAccess]) -> FSAccessSet:
    """Converts a sequence of filesystem accesses into sets of accesses.

    This tracks deleted files, assuming that a file that is written and
    then deleted is only a temporary, and is not counted as a final write.
    Reads of temporary files are allowed and not recorded.
    Deletes of files not written by this sequence of accesses are recorded.
    Converting from a stream to set(s) loses access sequence information.

    Args:
      accesses: stream of file-system accesses.

    Returns:
      Sets of read, written, and deleted files that should be verified
      elsewhere (excluding inferred temporaries).
    """
    reads = set()
    writes = set()
    deletes = set()
    for access in accesses:
        if access.op == FileAccessType.READ:
            # Reading a file that we've written is not interesting.
            # Omit those, but add all others.
            if access.path not in writes:
                reads.add(access.path)
        elif access.op == FileAccessType.WRITE:
            writes.add(access.path)
            deletes.discard(access.path)
        elif access.op == FileAccessType.DELETE:
            if access.path in writes:
                # Infer that this is a temporary file.
                writes.discard(access.path)
                # Allow and ignore reads to written files.
                reads.discard(access.path)
                # Do not record this as a deleted file.
            else:
                # All other deletes require scrutiny.
                deletes.add(access.path)

    # writes contains the set of files that were not deleted
    return FSAccessSet(reads=reads, writes=writes, deletes=deletes)


def check_access_permissions(
        accesses: FSAccessSet, constraints: AccessConstraints) -> FSAccessSet:
    """Checks a sequence of accesses against permission constraints.

    Args:
      accesses: sets of file-system read/write accesses.
      constraints: permitted accesses.
        .allowed_reads: set of files that are allowed to be read.
        .allowed_writes: set of files that are allowed to be written.

    Returns:
      Subset of not-permitted file accesses.
    """
    # Suppress diagnostics on reading files that are written,
    # regardless of whether or not those writes were allowed.
    # For example, temporarily written files (not declared as outputs)
    # should be allowed to be read without issue.
    allowed_reads = constraints.allowed_reads | accesses.writes
    unexpected_reads = accesses.reads - allowed_reads
    unexpected_writes = accesses.writes - constraints.allowed_writes
    return FSAccessSet(reads=unexpected_reads, writes=unexpected_writes)


def check_missing_writes(
        accesses: Iterable[FSAccess],
        required_writes: FrozenSet[str]) -> AbstractSet[str]:
    """Tracks sequence of access to verify that required files are written.

    Args:
      accesses: file-system accesses.
      required_writes: paths that are expected to be written.

    Returns:
      Subset of required_writes that were not fulfilled.
    """
    missing_writes = required_writes.copy()
    for access in accesses:
        if access.op == FileAccessType.WRITE and access.path in missing_writes:
            missing_writes.remove(access.path)
        elif access.op == FileAccessType.DELETE and access.path in required_writes:
            missing_writes.add(access.path)

    return missing_writes


def actually_read_files(accesses: Iterable[FSAccess]) -> AbstractSet[str]:
    """Returns subset of files that were actually used/read."""
    return {
        access.path for access in accesses if access.op == FileAccessType.READ
    }


def _verbose_path(path: str) -> str:
    """When any symlinks are followed, show this."""
    realpath = os.path.realpath(path)
    if path != realpath:
        return path + " -> " + realpath
    return path


@dataclasses.dataclass
class StalenessDiagnostics(object):
    """Just a structure to capture results of diagnosing outputs."""
    required_writes: FrozenSet[str] = dataclasses.field(default_factory=set)
    nonexistent_outputs: FrozenSet[str] = dataclasses.field(default_factory=set)
    # If there are stale_outputs, then it must have been compared against a
    # newest_input.
    newest_input: Optional[str] = None
    stale_outputs: FrozenSet[str] = dataclasses.field(default_factory=set)

    @property
    def has_findings(self):
        return self.nonexistent_outputs or self.stale_outputs

    def print_findings(self, stream: TextIO):
        """Prints human-readable diagnostics.

        Args:
          stream: a file stream, like sys.stderr.
        """
        required_writes_formatted = "\n".join(
            _verbose_path(f) for f in self.required_writes)
        print(
            f"""
Required writes:
{required_writes_formatted}
""", file=stream)
        if self.nonexistent_outputs:
            nonexistent_outputs_formatted = "\n".join(
                _verbose_path(f) for f in self.nonexistent_outputs)
            print(
                f"""
Missing outputs:
{nonexistent_outputs_formatted}
""",
                file=stream)

        if self.stale_outputs:
            stale_outputs_formatted = "\n".join(
                _verbose_path(f) for f in self.stale_outputs)
            print(
                f"""
Stale outputs: (older than newest input: {self.newest_input})
{stale_outputs_formatted}
""",
                file=stream)


def realpath_ctime(path: str) -> int:
    """Follow symlinks before getting ctime.

    This reflects Ninja's behavior of using `stat()` instead of `lstat()`
    on symlinks.

    Args:
      path: file or symlink

    Returns:
      ctime of the realpath of path.
    """
    return os.path.getctime(os.path.realpath(path))


def diagnose_stale_outputs(
        accesses: Iterable[FSAccess],
        access_constraints: AccessConstraints) -> StalenessDiagnostics:
    """Analyzes access stream for missing writes.

    Also compares timestamps of inputs relative to outputs
    to determine staleness.

    Args:
      accesses: trace of file system accesses.
      access_constraints: access that may/must[not] occur.

    Returns:
      Structure of findings, including missing/stale outputs.
    """
    # Verify that outputs are written as promised.
    missing_writes = check_missing_writes(
        accesses, access_constraints.required_writes)

    # Distinguish stale from nonexistent output files.
    untouched_outputs, nonexistent_outputs = _partition(
        missing_writes, os.path.exists)

    # Check that timestamps relative to inputs (allowed_reads) are newer,
    # in which case, not-writing outputs is acceptable.
    # Determines file use based on the `accesses` trace,
    # not the stat() filesystem function.
    read_files = actually_read_files(accesses)
    # Ignore allowed-but-unused inputs.
    # Outputs are readable, but should not be considered as inputs.
    used_inputs = access_constraints.inputs.intersection(read_files)

    # Compare timestamps vs. newest input to find stale outputs.
    stale_outputs = set()
    newest_input = None
    if used_inputs and untouched_outputs:
        # All links in inputs are followed to their destinations already in
        # previous steps, so realpath_ctime is unnecessary on them.
        newest_input = max(used_inputs, key=os.path.getctime)
        # Filter out untouched outputs that are still newer than used inputs.
        input_timestamp = os.path.getctime(newest_input)
        stale_outputs = {
            out for out in untouched_outputs
            if realpath_ctime(out) < input_timestamp
        }
    return StalenessDiagnostics(
        required_writes=access_constraints.required_writes,
        nonexistent_outputs=set(nonexistent_outputs),
        newest_input=newest_input,
        stale_outputs=stale_outputs)


def main_arg_parser() -> argparse.ArgumentParser:
    """Construct the argument parser, called by main()."""
    parser = argparse.ArgumentParser(
        description="Traces a GN action and enforces strict inputs/outputs",
        argument_default=[],
    )
    parser.add_argument(
        "--fsatrace-path",
        default="fsatrace",
        help=
        "Path to fsatrace binary.  If omitted, it will search for one in PATH.")
    parser.add_argument(
        "--label", required=True, help="The wrapped target's label")
    parser.add_argument(
        "--trace-output", required=True, help="Where to store the trace")
    parser.add_argument(
        "--target-type",
        choices=["action", "action_foreach"],
        default="action",
        help="Type of target being wrapped",
    )
    parser.add_argument("--inputs", nargs="*", help="action#inputs")
    parser.add_argument("--outputs", nargs="*", help="action#outputs")
    parser.add_argument("--depfile", help="action#depfile")

    parser.add_argument(
        "--failed-check-status",
        type=int,
        default=1,
        help=
        "On failing tracing checks, exit with this code.  Use 0 to report findings without failing.",
    )

    # Want --foo (default:True) and --no-foo (False).
    # This is ugly, trying to emulate argparse.BooleanOptionalAction,
    # which isn't available until Python 3.9.
    parser.add_argument(
        "--check-access-permissions",
        action="store_true",
        default=True,
        help="Check permissions on file reads and writes")
    parser.add_argument(
        "--no-check-access-permissions",
        action="store_false",
        dest="check_access_permissions")

    parser.add_argument(
        "--check-output-freshness",
        action="store_true",
        default=False,
        help="Check timestamp freshness of declared outputs")
    parser.add_argument(
        "--no-check-output-freshness",
        action="store_false",
        dest="check_output_freshness")

    # This affects the set of files that are allowed to be written.
    # TODO(fangism): remove this flag entirely, disallowing writes to inputs
    parser.add_argument(
        "--writeable-depfile-inputs",
        action="store_true",
        default=False,
        help=
        "Allow writes to inputs found in depfiles.  Only effective with --check-access-permissions."
    )
    parser.add_argument(
        "--no-writeable-depfile-inputs",
        action="store_false",
        dest="writeable_depfile_inputs")

    # TODO(fangism): This check is blocked on *.py being in the ignored set.
    parser.add_argument(
        "--check-inputs-not-in-ignored-set",
        action="store_true",
        default=False,  # Goal: always True (remove this flag)
        help="Check that inputs do not belong to the set of ignored files")

    # Positional args are the command (tool+args) to run and trace.
    parser.add_argument("command", nargs="*", help="action#command")
    return parser


def _tool_is_python(tool: str) -> bool:
    base = os.path.basename(tool)
    return base == "python" or base.startswith("python3")


def is_known_wrapper(command: ToolCommand) -> bool:
    """Is this a command-wrapping script?

    Returns:
      True if the command is one of the known wrapper scripts that encapsulates
        another command in tail position after '--'.
    """
    # Cover both cases when the tool:
    #
    # 1. is executed directly, for example: ./build.py
    if command.tool.endswith(('.py', '.pyz')):
        python_script = command.tool
    # 2. is explicitly executed by an interpreter
    #    for example: path/to/prebuilt/python3.8 build.py
    elif _tool_is_python(command.tool):
        script_index = _find_first_index(
            command.args, lambda x: x.endswith(('.py', '.pyz')))
        assert script_index != -1, f"Expected to find Python script after interpreter: {command.args}"
        python_script = command.args[script_index]
    else:
        return False

    if os.path.basename(python_script) in {"action_tracer.py",
                                           "output_cacher.py"}:
        return True

    return False


def main():
    parser = main_arg_parser()
    args = parser.parse_args()

    command = ToolCommand(tokens=args.command)

    # Unwrap certain command wrapper scripts.
    while is_known_wrapper(command):
        command = command.unwrap()

    # Identify the intended tool from the original command.
    script = command.tool

    # Ensure trace_output directory exists
    trace_output_dir = os.path.dirname(args.trace_output)
    os.makedirs(trace_output_dir, exist_ok=True)

    os.environ["FSAT_BUF_SIZE"] = "5000000"
    retval = subprocess.call(
        [
            args.fsatrace_path,
            "rwmdt",
            args.trace_output,
            "--",
        ] + command.tokens)

    # Scripts with known issues
    # TODO(shayba): file bugs for the suppressions below
    ignored_scripts = {
        # When using `/bin/ln -f`, a temporary file may be created in the
        # target directory. This will register as a write to a non-output file.
        # TODO(shayba): address this somehow.
        "ln",
        # fxbug.dev/61771
        # "analysis_options.yaml",
    }
    if os.path.basename(script) in ignored_scripts:
        return retval

    # Compute constraints from action properties (from args).
    action = Action(
        inputs=args.inputs, outputs=args.outputs, depfile=args.depfile)
    access_constraints = action.access_constraints(
        writeable_depfile_inputs=args.writeable_depfile_inputs)

    # Limit most access checks to files under src_root.
    src_root = os.path.dirname(os.path.dirname(os.getcwd()))

    # Paths that are ignored
    ignored_prefixes = {
        # Allow actions to access prebuilts that are not declared as inputs
        # (until we fix all instances of this)
        os.path.join(src_root, "prebuilt"),
        # Allow actions to run `git` commands.
        # Actions can set certain refs under .git as inputs to trigger on
        # relevant changes to git. However fully predicting what files will be
        # accessed by certain git commands used in the build is not viable, it's
        # not necessarily stable and doesn't make a good contract.
        os.path.join(src_root, ".git"),
        os.path.join(src_root, "integration", ".git"),
        os.path.join(src_root, "third_party", "mesa", ".git"),
        os.path.join(src_root, "third_party", "glslang", ".git"),
        # Allow actions to read .fx-build-dir to figure out the current build
        # directory.
        os.path.join(src_root, ".fx-build-dir"),
        # TODO(jayzhuang): flutter's dart_libraries currently don't have sources
        # listed, fix that and remove this exception.
        os.path.join(src_root, "third_party", "dart-pkg", "git", "flutter")
    }
    ignored_suffixes = {
        # TODO(fxb/71190): The following is a temporary symlink.
        # fsatrace fails to detect writing this symlink, and trace analysis
        # thinks it is a read that violates hermeticity, but it is ok.
        "src/github.com/pkg",
        # TODO(jayzhuang): Figure out whether `.dart_tool/package_config.json`
        # should be included in inputs.
        ".dart_tool/package_config.json",
        # Allow Flutter to read and write tool states.
        ".config/flutter/tool_state",
    }
    ignored_path_parts = {
        # Python creates these directories with bytecode caches
        "__pycache__",
        # fxbug.dev/68397: some actions are known to generate implicit outputs in
        # these directories that are unknown before the metadata collection phase.
        # It was decided to tolerate this behavior.
        "__shebang__",
        # This temporary directory is only used to find nonterministic outputs.
        ".tmp-repro",
    }
    # TODO(fangism): for suffixes that we always ignore for writing, such as
    # safe or intended side-effect byproducts, make sure no declared inputs ever
    # match them.

    raw_trace = ""
    with open(args.trace_output, "r") as trace:
        raw_trace = trace.read()

    # Parse trace file.
    all_accesses = parse_fsatrace_output(raw_trace.splitlines())

    # Ignore directory accesses, including symlinked dirs.
    # Files' contents are what matters for reproducibilty.
    file_accesses = [
        access for access in all_accesses
        if not os.path.isdir(os.path.realpath(access.path))
    ]

    # Filter out accesses we don't want to track.
    ignore_conditions = MatchConditions(
        prefixes=ignored_prefixes,
        suffixes=ignored_suffixes,
        components=ignored_path_parts,
    )

    exit_code = 0

    # Make sure no declared inputs match ignored patterns.
    # Ignored files should never be depended on by other actions.
    declared_ignored_inputs = {
        path for path in action.inputs if ignore_conditions.matches(path)
    }
    if args.check_inputs_not_in_ignored_set and declared_ignored_inputs:
        ignored_inputs_formatted = "\n  ".join(declared_ignored_inputs)
        print(
            f"""
The following inputs of {args.label} are ignored by action tracing, and thus,
should not be declared as dependencies.
  {ignored_inputs_formatted}

""",
            file=sys.stderr)
        exit_code = 1

    # Filter out access we don't want to track.
    filtered_accesses = [
        access for access in file_accesses if access.should_check(
            ignore_conditions=ignore_conditions,
            # Ignore accesses that fall outside of the source root.
            required_path_prefix=src_root,
        )
    ]

    file_access_sets = finalize_filesystem_accesses(filtered_accesses)

    # Check for overall correctness, print diagnostics,
    # and exit with the right code.
    if args.check_access_permissions:
        # Verify the filesystem access trace.
        unexpected_accesses = check_access_permissions(
            accesses=file_access_sets, constraints=access_constraints)

        if unexpected_accesses.all_accesses:
            unexpected_accesses_formatted = str(unexpected_accesses)
            print(
                f"""
Unexpected file accesses building {args.label}:
{unexpected_accesses_formatted}

Full access trace:
{raw_trace}

See: https://fuchsia.dev/fuchsia-src/development/build/hermetic_actions

""",
                file=sys.stderr)
            exit_code = args.failed_check_status

    if args.check_output_freshness:
        if retval == 0:
            # Action succeeded, make sure its outputs are fresh.
            output_diagnostics = diagnose_stale_outputs(
                accesses=filtered_accesses,
                access_constraints=access_constraints)
            if output_diagnostics.has_findings:

                print(
                    f"""
Not all outputs of {args.label} were written or touched, which can cause subsequent
build invocations to re-execute actions due to a missing file or old timestamp.
""",
                    file=sys.stderr)
                output_diagnostics.print_findings(sys.stderr)
                print(
                    f"""
Full access trace:
{raw_trace}

See: https://fuchsia.dev/fuchsia-src/development/build/ninja_no_op

""",
                    file=sys.stderr)
                exit_code = args.failed_check_status
        else:
            # Action failed.
            # Check that failed actions do not leave falsely up-to-date outputs
            # that would prevent them from being re-built incrementally.
            unexpected_fresh_outputs = access_constraints.fresh_outputs()
            if unexpected_fresh_outputs:
                outputs_formatted = "".join(
                    _verbose_path(f) for f in unexpected_fresh_outputs)
                print(
                    f"""
Action for {args.label} failed, yet the following outputs remain fresher than the newest input:

{outputs_formatted}

This may lead to a false assessment that the failed action is up-to-date.
                      """,
                    file=sys.stderr)
                # do not set the exit code

    if action.parsed_depfile:
        allowed_abspaths = {"/usr/bin/env"}
        abspaths = abspaths_from_depfile(
            action.parsed_depfile, allowed_abspaths)

        if abspaths:
            exit_code = args.failed_check_status
            one_path_per_line = '\n'.join(sorted(abspaths))
            print(
                f"""
Found the following files with absolute paths in depfile {action.depfile} for {args.label}:

{one_path_per_line}
""",
                file=sys.stderr)

    if retval != 0:
        # Always forward the action's non-zero exit code, regardless of tracer findings.
        return retval

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
