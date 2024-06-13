const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    //
    // C executable
    //
    const exe = b.addExecutable(.{
        .name = "kilo",
        // .root_source_file = b.path("src/main.c"),
        .target = target,
        .optimize = optimize,
    });
    exe.linkLibC();

    // Add files
    const exe_files = [_][]const u8{
        "src/kilo.c",
    };
    // Set flags
    const exe_flags = [_][]const u8{
        "-std=99",
        "-g",
        "-O0",
        "-Werror",
        "-Wall",
        "-Wextra",
        "-pedantic",
    };

    // Link libcpp to build a C++ app
    exe.linkLibC();
    // exe.linkSystemLibrary("sndfile");
    exe.addCSourceFiles(.{
        .files = &exe_files,
        .flags = &exe_flags,
    });
    exe.addIncludePath(b.path("include"));

    b.installArtifact(exe);

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    // This allows the user to pass arguments to the application in the build
    // command itself, like this: `zig build run -- arg1 arg2 etc`
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // This creates a build step. It will be visible in the `zig build --help` menu,
    // and can be selected like this: `zig build run`
    // This will evaluate the `run` step rather than the default, which is "install".
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // Creates a step for unit testing. This only builds the test executable
    // but does not run it.
    const lib_unit_tests = b.addTest(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const run_lib_unit_tests = b.addRunArtifact(lib_unit_tests);

    const exe_unit_tests = b.addTest(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    const run_exe_unit_tests = b.addRunArtifact(exe_unit_tests);

    // Similar to creating the run step earlier, this exposes a `test` step to
    // the `zig build --help` menu, providing a way for the user to request
    // running the unit tests.
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_lib_unit_tests.step);
    test_step.dependOn(&run_exe_unit_tests.step);
}
