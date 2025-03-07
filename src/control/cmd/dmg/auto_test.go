//
// (C) Copyright 2020-2022 Intel Corporation.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

package main

import (
	"context"
	"fmt"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"github.com/pkg/errors"
	"gopkg.in/yaml.v2"

	ctlpb "github.com/daos-stack/daos/src/control/common/proto/ctl"
	"github.com/daos-stack/daos/src/control/common/test"
	"github.com/daos-stack/daos/src/control/lib/control"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/security"
	"github.com/daos-stack/daos/src/control/server/config"
	"github.com/daos-stack/daos/src/control/server/engine"
	"github.com/daos-stack/daos/src/control/server/storage"
)

func TestAuto_ConfigCommands(t *testing.T) {
	runCmdTests(t, []cmdTest{
		{
			"Generate with no access point",
			"config generate",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with defaults",
			"config generate -a foo",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with no nvme",
			"config generate -a foo --min-ssds 0",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with storage parameters",
			"config generate -a foo --num-engines 2 --min-ssds 4",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with short option storage parameters",
			"config generate -a foo -e 2 -s 4",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with ethernet network device class",
			"config generate -a foo --net-class ethernet",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with infiniband network device class",
			"config generate -a foo --net-class infiniband",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("no host responses"),
		},
		{
			"Generate with deprecated network device class",
			"config generate -a foo --net-class best-available",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("Invalid value"),
		},
		{
			"Generate with unsupported network device class",
			"config generate -a foo --net-class loopback",
			strings.Join([]string{
				printRequest(t, &control.NetworkScanReq{}),
			}, " "),
			errors.New("Invalid value"),
		},
		{
			"Nonexistent subcommand",
			"network quack",
			"",
			errors.New("Unknown command"),
		},
	})
}

// The Control API calls made in configGenCmd.confGen() are already well tested so just do some
// sanity checking here to prevent regressions.
func TestAuto_confGen(t *testing.T) {
	ib0 := &ctlpb.FabricInterface{
		Provider: "ofi+psm2", Device: "ib0", Numanode: 0, Netdevclass: 32, Priority: 0,
	}
	ib1 := &ctlpb.FabricInterface{
		Provider: "ofi+psm2", Device: "ib1", Numanode: 1, Netdevclass: 32, Priority: 1,
	}
	netHostResp := &control.HostResponse{
		Addr: "host1",
		Message: &ctlpb.NetworkScanResp{
			Numacount:    2,
			Corespernuma: 24,
			Interfaces:   []*ctlpb.FabricInterface{ib0, ib1},
		},
	}
	storHostResp := &control.HostResponse{
		Addr:    "host1",
		Message: control.MockServerScanResp(t, "withSpaceUsage"),
	}
	exmplEngineCfg := control.DefaultEngineCfg(0).
		WithPinnedNumaNode(0).
		WithFabricInterface("ib0").
		WithFabricInterfacePort(31416).
		WithFabricProvider("ofi+psm2").
		WithStorage(
			storage.NewTierConfig().
				WithNumaNodeIndex(0).
				WithStorageClass(storage.ClassDcpm.String()).
				WithScmDeviceList("/dev/pmem0").
				WithScmMountPoint("/mnt/daos0"),
			storage.NewTierConfig().
				WithNumaNodeIndex(0).
				WithStorageClass(storage.ClassNvme.String()).
				WithBdevDeviceList(
					storage.MockNvmeController(2).PciAddr,
					storage.MockNvmeController(4).PciAddr,
					storage.MockNvmeController(6).PciAddr,
					storage.MockNvmeController(8).PciAddr),
		).
		WithTargetCount(16).
		WithHelperStreamCount(4)
	exmplEngineCfgs := []*engine.Config{
		exmplEngineCfg,
		control.DefaultEngineCfg(1).
			WithPinnedNumaNode(1).
			WithFabricInterface("ib1").
			WithFabricInterfacePort(32416).
			WithFabricProvider("ofi+psm2").
			WithFabricNumaNodeIndex(1).
			WithStorage(
				storage.NewTierConfig().
					WithNumaNodeIndex(1).
					WithStorageClass(storage.ClassDcpm.String()).
					WithScmDeviceList("/dev/pmem1").
					WithScmMountPoint("/mnt/daos1"),
				storage.NewTierConfig().
					WithNumaNodeIndex(1).
					WithStorageClass(storage.ClassNvme.String()).
					WithBdevDeviceList(
						storage.MockNvmeController(1).PciAddr,
						storage.MockNvmeController(3).PciAddr,
						storage.MockNvmeController(5).PciAddr,
						storage.MockNvmeController(7).PciAddr),
			).
			WithStorageNumaNodeIndex(1).
			WithTargetCount(16).
			WithHelperStreamCount(4),
	}
	baseConfig := func(prov string, ecs []*engine.Config) *config.Server {
		for idx, ec := range ecs {
			ec.WithStorageConfigOutputPath(fmt.Sprintf("/mnt/daos%d/daos_nvme.conf", idx)).
				WithStorageVosEnv("NVME")
		}
		return config.DefaultServer().
			WithControlLogFile("").
			WithFabricProvider(prov).
			WithDisableVMD(false).
			WithEngines(ecs...)
	}

	for name, tc := range map[string]struct {
		hostlist         []string
		accessPoints     string
		nrEngines        int
		minNrSSDs        int
		netClass         string
		uErr             error
		hostResponsesSet [][]*control.HostResponse
		expCfg           *config.Server
		expErr           error
	}{
		"no host responses": {
			expErr: errors.New("no host responses"),
		},
		"fetching host unary error": {
			uErr:   errors.New("bad fetch"),
			expErr: errors.New("bad fetch"),
		},
		"fetching host fabric error": {
			hostResponsesSet: [][]*control.HostResponse{
				{&control.HostResponse{Addr: "host1", Error: errors.New("server fail")}},
			},
			expErr: errors.New("1 host"),
		},
		"fetching host storage error": {
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{&control.HostResponse{Addr: "host1", Error: errors.New("server fail")}},
			},
			expErr: errors.New("1 host"),
		},
		"successful fetch of host storage and fabric": {
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{storHostResp},
			},
			expCfg: baseConfig("ofi+psm2", exmplEngineCfgs).
				WithNrHugePages(16384).
				WithAccessPoints("localhost:10001").
				WithControlLogFile("/tmp/daos_server.log"),
		},
		"successful fetch of host storage and fabric; access points set": {
			accessPoints: "moon-111,mars-115,jupiter-119",
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{storHostResp},
			},
			expCfg: baseConfig("ofi+psm2", exmplEngineCfgs).
				WithNrHugePages(16384).
				WithAccessPoints("moon-111:10001", "mars-115:10001", "jupiter-119:10001").
				WithControlLogFile("/tmp/daos_server.log"),
		},
		"successful fetch of host storage and fabric; unmet min nr ssds": {
			minNrSSDs: 8,
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{storHostResp},
			},
			expErr: errors.New("insufficient number of ssds"),
		},
		"successful fetch of host storage and fabric; unmet nr engines": {
			nrEngines: 8,
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{storHostResp},
			},
			expErr: errors.New("insufficient number of pmem"),
		},
		"successful fetch of host storage and fabric; bad net class": {
			netClass: "foo",
			hostResponsesSet: [][]*control.HostResponse{
				{netHostResp},
				{storHostResp},
			},
			expErr: errors.New("unrecognized net-class"),
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(name)
			defer test.ShowBufferOnFailure(t, buf)

			// Mimic go-flags default values.
			if tc.minNrSSDs == 0 {
				tc.minNrSSDs = 1
			}
			if tc.netClass == "" {
				tc.netClass = "infiniband"
			}
			if tc.accessPoints == "" {
				tc.accessPoints = "localhost"
			}

			cmd := &configGenCmd{
				AccessPoints: tc.accessPoints,
				NrEngines:    tc.nrEngines,
				MinNrSSDs:    tc.minNrSSDs,
				NetClass:     tc.netClass,
			}
			cmd.Logger = log
			cmd.hostlist = tc.hostlist

			if tc.hostResponsesSet == nil {
				tc.hostResponsesSet = [][]*control.HostResponse{{}}
			}
			mic := control.MockInvokerConfig{}
			for _, r := range tc.hostResponsesSet {
				mic.UnaryResponseSet = append(mic.UnaryResponseSet,
					&control.UnaryResponse{Responses: r})
			}
			mic.UnaryError = tc.uErr
			cmd.ctlInvoker = control.NewMockInvoker(log, &mic)

			gotCfg, gotErr := cmd.confGen(context.TODO())
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			cmpOpts := []cmp.Option{
				cmp.Comparer(func(x, y *storage.BdevDeviceList) bool {
					if x == nil && y == nil {
						return true
					}
					return x.Equals(y)
				}),
				cmpopts.IgnoreUnexported(security.CertificateConfig{}),
			}

			if diff := cmp.Diff(tc.expCfg, gotCfg, cmpOpts...); diff != "" {
				t.Fatalf("unexpected config generated (-want, +got):\n%s\n", diff)
			}
		})
	}
}

// TestAuto_ConfigWrite verifies that output from config generate command matches documented
// parameters in utils/config/daos_server.yml and that private parameters are not displayed.
// Typical auto-generated output is taken from src/control/lib/control/auto_test.go.
func TestAuto_ConfigWrite(t *testing.T) {
	const (
		defaultFiPort         = 31416
		defaultFiPortInterval = 1000
		defaultTargetCount    = 16
		defaultEngineLogFile  = "/tmp/daos_engine"
		defaultControlLogFile = "/tmp/daos_server.log"

		expOut = `port: 10001
transport_config:
  allow_insecure: false
  client_cert_dir: /etc/daos/certs/clients
  ca_cert: /etc/daos/certs/daosCA.crt
  cert: /etc/daos/certs/server.crt
  key: /etc/daos/certs/server.key
engines:
- targets: 12
  nr_xs_helpers: 2
  first_core: 0
  log_file: /tmp/daos_engine.0.log
  storage:
  - class: dcpm
    scm_mount: /mnt/daos0
    scm_list:
    - /dev/pmem0
  - class: nvme
    bdev_list:
    - "0000:00:00.0"
    - "0000:01:00.0"
    - "0000:02:00.0"
    - "0000:03:00.0"
  provider: ofi+verbs
  fabric_iface: ib0
  fabric_iface_port: 31416
  pinned_numa_node: 0
- targets: 6
  nr_xs_helpers: 0
  first_core: 0
  log_file: /tmp/daos_engine.1.log
  storage:
  - class: dcpm
    scm_mount: /mnt/daos1
    scm_list:
    - /dev/pmem1
  - class: nvme
    bdev_list:
    - "0000:04:00.0"
    - "0000:05:00.0"
    - "0000:06:00.0"
  provider: ofi+verbs
  fabric_iface: ib1
  fabric_iface_port: 32416
  pinned_numa_node: 1
disable_vfio: false
disable_vmd: false
enable_hotplug: false
nr_hugepages: 6144
disable_hugepages: false
control_log_mask: INFO
control_log_file: /tmp/daos_server.log
core_dump_filter: 19
name: daos_server
socket_dir: /var/run/daos_server
provider: ofi+verbs
access_points:
- hostX:10002
fault_cb: ""
hyperthreads: false
`
	)

	typicalAutoGenOutCfg := config.DefaultServer().
		WithControlLogFile(defaultControlLogFile).
		WithFabricProvider("ofi+verbs").
		WithAccessPoints("hostX:10002").
		WithNrHugePages(6144).
		WithDisableVMD(false).
		WithEngines(
			engine.MockConfig().
				WithTargetCount(defaultTargetCount).
				WithLogFile(fmt.Sprintf("%s.%d.log", defaultEngineLogFile, 0)).
				WithFabricInterface("ib0").
				WithFabricInterfacePort(defaultFiPort).
				WithFabricProvider("ofi+verbs").
				WithPinnedNumaNode(0).
				WithStorage(
					storage.NewTierConfig().
						WithStorageClass(storage.ClassDcpm.String()).
						WithScmDeviceList("/dev/pmem0").
						WithScmMountPoint("/mnt/daos0"),
					storage.NewTierConfig().
						WithStorageClass(storage.ClassNvme.String()).
						WithBdevDeviceList(test.MockPCIAddrs(0, 1, 2, 3)...),
				).
				WithStorageConfigOutputPath("/mnt/daos0/daos_nvme.conf").
				WithStorageVosEnv("NVME").
				WithTargetCount(12).
				WithHelperStreamCount(2),
			engine.MockConfig().
				WithTargetCount(defaultTargetCount).
				WithLogFile(fmt.Sprintf("%s.%d.log", defaultEngineLogFile, 1)).
				WithFabricInterface("ib1").
				WithFabricInterfacePort(
					int(defaultFiPort+defaultFiPortInterval)).
				WithFabricProvider("ofi+verbs").
				WithPinnedNumaNode(1).
				WithStorage(
					storage.NewTierConfig().
						WithStorageClass(storage.ClassDcpm.String()).
						WithScmDeviceList("/dev/pmem1").
						WithScmMountPoint("/mnt/daos1"),
					storage.NewTierConfig().
						WithStorageClass(storage.ClassNvme.String()).
						WithBdevDeviceList(test.MockPCIAddrs(4, 5, 6)...),
				).
				WithStorageConfigOutputPath("/mnt/daos1/daos_nvme.conf").
				WithStorageVosEnv("NVME").
				WithTargetCount(6).
				WithHelperStreamCount(0),
		)

	bytes, err := yaml.Marshal(typicalAutoGenOutCfg)
	if err != nil {
		t.Fatal(err)
	}

	if diff := cmp.Diff(expOut, string(bytes)); diff != "" {
		t.Fatalf("unexpected output config (-want, +got):\n%s\n", diff)
	}
}
