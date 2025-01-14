// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"go.fuchsia.dev/fuchsia/tools/sdk-tools/sdkcommon"
)

type testSDKProperties struct {
	dataPath        string
	err             error
	properties      map[string]string
	expectedFfxArgs []string
	ffxCalled       *bool
}

func (testSDK testSDKProperties) GetToolsDir() (string, error) {
	return "fake-tools", nil
}

func (testSDK testSDKProperties) GetFuchsiaProperty(deviceName string, property string) (string, error) {
	if testSDK.err != nil {
		return "", testSDK.err
	}
	var key string
	if deviceName != "" {
		key = fmt.Sprintf("%v.%v", deviceName, property)
	} else {
		key = property
	}
	return testSDK.properties[key], nil
}

func (testSDK testSDKProperties) ResolveTargetAddress(deviceIP string, deviceName string) (sdkcommon.DeviceConfig, error) {
	return sdkcommon.DeviceConfig{
		DeviceName: deviceName,
		DeviceIP:   deviceIP,
	}, nil
}

func (testSDK testSDKProperties) RunFFX(args []string, interactive bool) (string, error) {
	if !reflect.DeepEqual(args, testSDK.expectedFfxArgs) {
		fmt.Fprintf(os.Stderr, "Argument mismatch.\n")
		fmt.Fprintf(os.Stderr, "Expected: %v\n", testSDK.expectedFfxArgs)
		fmt.Fprintf(os.Stderr, "Actual  : %v\n", args)
		os.Exit(1)
	}

	*testSDK.ffxCalled = true
	return "", nil
}

// See exec_test.go for details, but effectively this runs the function called TestHelperProcess passing
// the args.
func helperCommandForFPublish(command string, s ...string) (cmd *exec.Cmd) {

	cs := []string{"-test.run=TestFakeFPublish", "--"}
	cs = append(cs, command)
	cs = append(cs, s...)
	cmd = exec.Command(os.Args[0], cs...)
	// Set this in the enviroment, so we can control the result.
	cmd.Env = append(os.Environ(), "GO_WANT_HELPER_PROCESS=1")
	return cmd
}

func TestFakeFPublish(t *testing.T) {
	t.Helper()
	if os.Getenv("GO_WANT_HELPER_PROCESS") != "1" {
		return
	}
	defer os.Exit(0)

	args := os.Args
	for len(args) > 0 {
		if args[0] == "--" {
			args = args[1:]
			break
		}
		args = args[1:]
	}
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "No command\n")
		os.Exit(2)
	}
	// Check the command line
	cmd, args := args[0], args[1:]
	if filepath.Base(cmd) == "ffx" {
		handleFakeFFX(args)
	} else if filepath.Base(cmd) != "pm" {
		fmt.Fprintf(os.Stderr, "Unexpected command %v, expected 'pm'", cmd)
		os.Exit(1)
	}
	expected := strings.Split(os.Getenv("TEST_EXPECTED_ARGS"), ",")

	if len(args) != len(expected) {
		fmt.Fprintf(os.Stderr, "Argument count mismatch. Expected %v, actual: %v\n", len(args), len(expected))
		fmt.Fprintf(os.Stderr, "Expected: %v\n", expected)
		fmt.Fprintf(os.Stderr, "Actual  : %v\n", args)
		os.Exit(1)
	}
	for i := range args {
		if args[i] != expected[i] {
			fmt.Fprintf(os.Stderr,
				"Mismatched args index %v. Expected: %v actual: %v\n",
				i, expected[i], args[i])
			fmt.Fprintf(os.Stderr, "Full args Expected: %v actual: %v",
				expected, args)
			os.Exit(3)
		}
	}

	os.Exit(0)
}

func handleFakeFFX(args []string) {
	if args[0] == "config" && args[1] == "env" {
		if len(args) == 3 && args[2] == "get" {
			fmt.Printf("Environment:\n")
			fmt.Printf("User: none\n")
			fmt.Printf("Build: none\n")
			fmt.Printf("Global: none\n")
			os.Exit(0)
		} else if args[2] == "set" {
			os.Exit(0)
		}
	}
	if args[0] == "config" && args[1] == "get" {
		if args[2] == "DeviceConfiguration" {
			fmt.Printf(os.Getenv("_FAKE_FFX_DEVICE_CONFIG_DATA"))
			os.Exit(0)
		} else if args[2] == "DeviceConfiguration._DEFAULT_DEVICE_" {
			fmt.Printf(os.Getenv("_FAKE_FFX_DEVICE_CONFIG_DEFAULT_DEVICE"))
			os.Exit(0)
		} else if args[2] == "DeviceConfiguration.remote-target-name" {
			fmt.Println(`{"bucket":"","device-ip":"","device-name":"remote-target-name","image":"","package-port":"","package-repo":"/some/custom/repo/path","ssh-port":""}`)
			os.Exit(0)
		}

	}
	if args[0] == "target" && args[1] == "default" && args[2] == "get" {
		fmt.Printf("%v\n", os.Getenv("_FAKE_FFX_TARGET_DEFAULT"))
		os.Exit(0)
	}

	if args[0] == "target" && args[1] == "list" && args[2] == "--format" && args[3] == "json" {
		fmt.Printf("%v\n", os.Getenv("_FAKE_FFX_TARGET_LIST"))
		os.Exit(0)
	}

	if args[0] == "debug" && args[1] == "symbol-index" && args[2] == "add" {
		os.Exit(0)
	}
	fmt.Fprintf(os.Stderr, "Unexpected ffx sub command: %v", args)
	os.Exit(2)
}

func TestMain(t *testing.T) {
	dataDir := t.TempDir()
	savedArgs := os.Args
	savedCommandLine := flag.CommandLine
	ExecCommand = helperCommandForFPublish
	sdkcommon.ExecCommand = helperCommandForFPublish
	defer func() {
		ExecCommand = exec.Command
		sdkcommon.ExecCommand = exec.Command
		os.Args = savedArgs
		flag.CommandLine = savedCommandLine
	}()
	tests := []struct {
		args                []string
		deviceConfiguration string
		defaultConfigDevice string
		ffxDefaultDevice    string
		ffxTargetList       string
		ffxTargetDefault    string
		expectedArgs        []string
	}{
		// Test case for no configuration, but 1 device discoverable.
		{
			args:          []string{os.Args[0], "-data-path", dataDir, "package.far"},
			expectedArgs:  []string{"publish", "-n", "-a", "-r", filepath.Join(dataDir, "<unknown>", "packages/amber-files"), "-f", "package.far"},
			ffxTargetList: `[{"nodename":"<unknown>","rcs_state":"N","serial":"<unknown>","target_type":"Unknown","target_state":"Product","addresses":["::1f"]}]`,
		},
		{
			args:          []string{os.Args[0], "-data-path", dataDir, "--device-name", "test-device", "package.far"},
			expectedArgs:  []string{"publish", "-n", "-a", "-r", filepath.Join(dataDir, "test-device", "packages/amber-files"), "-f", "package.far"},
			ffxTargetList: `[{"nodename":"<unknown>","rcs_state":"N","serial":"<unknown>","target_type":"Unknown","target_state":"Product","addresses":["::1f"]}]`,
			deviceConfiguration: `{ "_DEFAULT_DEVICE_":"remote-target-name",
			"remote-target-name":{
				"bucket":"fuchsia-bucket",
				"device-ip":"::1f",
				"device-name":"remote-target-name",
				"image":"release",
				"package-port":"",
				"package-repo":"",
				"ssh-port":"2202",
				"default": "true"},
				"test-device":{
					"bucket":"fuchsia-bucket",
					"device-ip":"::ff",
					"device-name":"test-device",
					"image":"release",
					"package-port":"",
					"package-repo":"",
					"ssh-port":"",
					"default": "true"}
			}`,
			defaultConfigDevice: "\"remote-target-name\"",
		},
		{
			args:          []string{os.Args[0], "-data-path", dataDir, "--device-name", "test-device", "package.far"},
			expectedArgs:  []string{"publish", "-n", "-a", "-r", filepath.Join(dataDir, "test-device", "packages/amber-files"), "-f", "package.far"},
			ffxTargetList: `[{"nodename":"test-device","rcs_state":"N","serial":"<unknown>","target_type":"Unknown","target_state":"Product","addresses":["::1f"]}]`,
			deviceConfiguration: `{ "_DEFAULT_DEVICE_":"remote-target-name",
			"remote-target-name":{
				"bucket":"fuchsia-bucket",
				"device-ip":"::1f",
				"device-name":"remote-target-name",
				"image":"release",
				"package-port":"",
				"package-repo":"",
				"ssh-port":"2202",
				"default": "true"},
				"test-device2":{
					"bucket":"fuchsia-bucket",
					"device-ip":"::ff",
					"device-name":"test-device2",
					"image":"release",
					"package-port":"",
					"package-repo":"",
					"ssh-port":"",
					"default": "true"}
			}`,
			defaultConfigDevice: "\"remote-target-name\"",
		},
		{
			args:          []string{os.Args[0], "-data-path", dataDir, "package.far"},
			expectedArgs:  []string{"publish", "-n", "-a", "-r", "/some/custom/repo/path", "-f", "package.far"},
			ffxTargetList: `[{"nodename":"<unknown>","rcs_state":"N","serial":"<unknown>","target_type":"Unknown","target_state":"Product","addresses":["::1f"]}]`,
			deviceConfiguration: `{ "_DEFAULT_DEVICE_":"remote-target-name",
			"remote-target-name":{
				"bucket":"fuchsia-bucket",
				"device-ip":"::1f",
				"device-name":"remote-target-name",
				"image":"release",
				"package-port":"",
				"package-repo":"/some/custom/repo/path",
				"ssh-port":"2202",
				"default": "true"},
				"test-device":{
					"bucket":"fuchsia-bucket",
					"device-ip":"::ff",
					"device-name":"test-device",
					"image":"release",
					"package-port":"",
					"package-repo":"",
					"ssh-port":"",
					"default": "true"}
			}`,
			defaultConfigDevice: "\"remote-target-name\"",
		},
	}
	for testcase, test := range tests {
		t.Run(fmt.Sprintf("testcase_%d", testcase), func(t *testing.T) {
			os.Args = test.args
			os.Setenv("TEST_EXPECTED_ARGS", strings.Join(test.expectedArgs, ","))
			os.Setenv("_FAKE_FFX_DEVICE_CONFIG_DATA", test.deviceConfiguration)
			os.Setenv("_FAKE_FFX_DEVICE_CONFIG_DEFAULT_DEVICE", test.defaultConfigDevice)
			os.Setenv("_FAKE_FFX_TARGET_DEFAULT", test.ffxTargetDefault)
			os.Setenv("_FAKE_FFX_TARGET_LIST", test.ffxTargetList)
			flag.CommandLine = flag.NewFlagSet(os.Args[0], flag.ExitOnError)
			osExit = func(code int) {
				if code != 0 {
					t.Fatalf("Non-zero error code %d", code)
				}
			}
			main()
		})
	}
}

func TestFPublish(t *testing.T) {
	ExecCommand = helperCommandForFPublish
	defer func() { ExecCommand = exec.Command }()
	tests := []struct {
		repoDir      string
		deviceName   string
		packages     []string
		properties   map[string]string
		expectedArgs []string
	}{
		{
			repoDir:    "",
			deviceName: "",
			packages:   []string{"package.far"},
			properties: map[string]string{
				sdkcommon.PackageRepoKey: "/fake/repo/amber-files",
			},
			expectedArgs: []string{"publish", "-n", "-a", "-r", "/fake/repo/amber-files", "-f", "package.far"},
		},
		{
			repoDir:    "",
			deviceName: "test-device",
			packages:   []string{"package.far"},
			properties: map[string]string{
				sdkcommon.PackageRepoKey:                                "/fake/repo/amber-files",
				fmt.Sprintf("test-device.%v", sdkcommon.PackageRepoKey): "/fake/test-device/repo/amber-files",
			},
			expectedArgs: []string{"publish", "-n", "-a", "-r", "/fake/test-device/repo/amber-files", "-f", "package.far"},
		},
		{
			repoDir:      "/fake/repo/amber-files",
			deviceName:   "",
			packages:     []string{"package.far"},
			expectedArgs: []string{"publish", "-n", "-a", "-r", "/fake/repo/amber-files", "-f", "package.far"},
		},
		{
			repoDir:    "/fake/repo/amber-files",
			deviceName: "some-device",
			properties: map[string]string{
				sdkcommon.PackageRepoKey:                                "/invalid/repo/amber-files",
				fmt.Sprintf("some-device.%v", sdkcommon.PackageRepoKey): "/fake/some-device/repo/amber-files",
			},
			packages:     []string{"package.far"},
			expectedArgs: []string{"publish", "-n", "-a", "-r", "/fake/repo/amber-files", "-f", "package.far"},
		},
	}
	for i, test := range tests {
		os.Setenv("TEST_EXPECTED_ARGS", strings.Join(test.expectedArgs, ","))
		testSDK := testSDKProperties{dataPath: "/fake",
			properties: test.properties}
		t.Run(fmt.Sprintf("Test case %d", i), func(t *testing.T) {
			output, err := publish(testSDK, test.repoDir, test.deviceName, test.packages, false)
			if err != nil {
				t.Errorf("Error running fpublish: %v: %v",
					output, err)
			}
		})

	}
}

func TestRegisterSymbolIndex(t *testing.T) {
	tempDir := t.TempDir()
	farFile := filepath.Join(tempDir, "some_package.far")
	symbolIndexJsonFile := filepath.Join(tempDir, "some_package.symbol-index.json")
	os.WriteFile(symbolIndexJsonFile, []byte("{}"), 0666)
	ffxCalled := false
	testSDK := testSDKProperties{
		expectedFfxArgs: []string{"debug", "symbol-index", "add", symbolIndexJsonFile},
		ffxCalled:       &ffxCalled,
	}
	registerSymbolIndex(&testSDK, []string{farFile}, false)
	if !ffxCalled {
		t.Fatal("ffx should be called")
	}
	ffxCalled = false
	registerSymbolIndex(&testSDK, []string{"another_nonexist_package.far"}, false)
	if ffxCalled {
		t.Fatal("ffx should not be called")
	}
}
