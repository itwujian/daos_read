//
// (C) Copyright 2020-2022 Intel Corporation.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

package control

import (
	"context"
	"fmt"
	"sort"
	"testing"

	"github.com/dustin/go-humanize"
	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"github.com/pkg/errors"

	"github.com/daos-stack/daos/src/control/common/proto/convert"
	ctlpb "github.com/daos-stack/daos/src/control/common/proto/ctl"
	"github.com/daos-stack/daos/src/control/common/test"
	"github.com/daos-stack/daos/src/control/lib/hardware"
	"github.com/daos-stack/daos/src/control/lib/hostlist"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/security"
	"github.com/daos-stack/daos/src/control/server/config"
	"github.com/daos-stack/daos/src/control/server/engine"
	"github.com/daos-stack/daos/src/control/server/storage"
)

var (
	ib0 = &HostFabricInterface{
		Provider: "ofi+psm2", Device: "ib0", NumaNode: 0, NetDevClass: 32, Priority: 0,
	}
	ib1 = &HostFabricInterface{
		Provider: "ofi+psm2", Device: "ib1", NumaNode: 1, NetDevClass: 32, Priority: 1,
	}
	ib0r = &HostFabricInterface{
		Provider: "ofi+psm2", Device: "ib0", NumaNode: 0, NetDevClass: 32, Priority: 1,
	}
	ib1r = &HostFabricInterface{
		Provider: "ofi+psm2", Device: "ib1", NumaNode: 1, NetDevClass: 32, Priority: 0,
	}
	eth0 = &HostFabricInterface{
		Provider: "ofi+tcp", Device: "eth0", NumaNode: 0, NetDevClass: 1, Priority: 2,
	}
	eth1 = &HostFabricInterface{
		Provider: "ofi+tcp", Device: "eth1", NumaNode: 1, NetDevClass: 1, Priority: 3,
	}
	if1PB = &ctlpb.FabricInterface{
		Provider: "test-provider", Device: "test-device", Numanode: 42,
	}
	if2PB = &ctlpb.FabricInterface{
		Provider: "test-provider", Device: "test-device2", Numanode: 84,
	}
	fabIfs1            = &ctlpb.NetworkScanResp{Interfaces: []*ctlpb.FabricInterface{if1PB, if2PB}}
	hostRespRemoteFail = []*HostResponse{
		{Addr: "host1", Message: fabIfs1},
		{Addr: "host2", Error: errors.New("remote failed"), Message: fabIfs1}}
	hostRespRemoteFails = []*HostResponse{
		{Addr: "host1", Error: errors.New("remote failed"), Message: fabIfs1},
		{Addr: "host2", Error: errors.New("remote failed"), Message: fabIfs1},
	}
	fabIfs1wNuma = &ctlpb.NetworkScanResp{
		Interfaces: []*ctlpb.FabricInterface{if1PB, if2PB}, Numacount: 2,
	}
	fabIfs2 = &ctlpb.NetworkScanResp{Interfaces: []*ctlpb.FabricInterface{if2PB}}
	typIfs  = []*ctlpb.FabricInterface{
		{Provider: "ofi+psm2", Device: "ib1", Numanode: 1, Priority: 0, Netdevclass: 32},
		{Provider: "ofi+psm2", Device: "ib0", Numanode: 0, Priority: 1, Netdevclass: 32},
		{Provider: "ofi+verbs;ofi_rxm", Device: "ib1", Numanode: 1, Priority: 2, Netdevclass: 32},
		{Provider: "ofi+verbs;ofi_rxm", Device: "ib0", Numanode: 0, Priority: 3, Netdevclass: 32},
		{Provider: "ofi+verbs;ofi_rxm", Device: "eth0", Numanode: 0, Priority: 4, Netdevclass: 1},
		{Provider: "ofi+tcp;ofi_rxm", Device: "ib1", Numanode: 1, Priority: 5, Netdevclass: 32},
		{Provider: "ofi+tcp;ofi_rxm", Device: "ib0", Numanode: 0, Priority: 6, Netdevclass: 32},
		{Provider: "ofi+tcp;ofi_rxm", Device: "eth0", Numanode: 0, Priority: 7, Netdevclass: 1},
		{Provider: "ofi+verbs", Device: "ib1", Numanode: 1, Priority: 8, Netdevclass: 32},
		{Provider: "ofi+verbs", Device: "ib0", Numanode: 0, Priority: 9, Netdevclass: 32},
		{Provider: "ofi+tcp", Device: "ib1", Numanode: 1, Priority: 10, Netdevclass: 32},
		{Provider: "ofi+tcp", Device: "ib0", Numanode: 0, Priority: 11, Netdevclass: 32},
		{Provider: "ofi+tcp", Device: "eth0", Numanode: 0, Priority: 12, Netdevclass: 1},
		{Provider: "ofi+sockets", Device: "ib1", Numanode: 1, Priority: 13, Netdevclass: 32},
		{Provider: "ofi+sockets", Device: "ib0", Numanode: 0, Priority: 14, Netdevclass: 32},
		{Provider: "ofi+sockets", Device: "eth0", Numanode: 0, Priority: 15, Netdevclass: 1},
	}
	dualHostResp = func(r1, r2 *ctlpb.NetworkScanResp) []*HostResponse {
		return []*HostResponse{
			{
				Addr:    "host1",
				Message: r1,
			},
			{
				Addr:    "host2",
				Message: r2,
			},
		}
	}
	dualHostRespSame = func(r1 *ctlpb.NetworkScanResp) []*HostResponse {
		return dualHostResp(r1, r1)
	}
)

var pciSetCmpOpts = append([]cmp.Option{
	cmp.Comparer(func(x, y *hardware.PCIAddressSet) bool {
		if x == nil && y == nil {
			return true
		}
		return x.String() == y.String()
	}),
}, defResCmpOpts()...)

func pbIfs2ProvMap(t *testing.T, ifs []*ctlpb.FabricInterface, ndc hardware.NetDevClass) providerIfaceMap {
	t.Helper()

	log, buf := logging.NewTestLogger(t.Name())
	defer test.ShowBufferOnFailure(t, buf)

	sr := &ctlpb.NetworkScanResp{Interfaces: ifs, Numacount: 2, Corespernuma: 24}

	ns, err := fabricFromHostResp(t, log, nil, []*HostResponse{{Message: sr}})
	if err != nil {
		t.Fatal(err)
	}

	nd, err := getNetworkDetails(log, ndc, "", ns.HostFabric)
	if err != nil {
		t.Fatal(err)
	}

	return nd.ProviderIfaces
}

func fabricFromHostResp(t *testing.T, log logging.Logger, uErr error, hostResponses []*HostResponse) (*HostFabricSet, error) {
	t.Helper()

	mi := NewMockInvoker(log, &MockInvokerConfig{
		UnaryError: uErr,
		UnaryResponse: &UnaryResponse{
			Responses: hostResponses,
		},
	})

	return getNetworkSet(context.TODO(), log, []string{}, mi)
}

func cmpHostErrs(t *testing.T, expErrs []*MockHostError, gotErrs *HostErrorsResp) {
	t.Helper()

	if expErrs != nil {
		expHostErrs := MockHostErrorsResp(t, expErrs...)
		if diff := cmp.Diff(expHostErrs.GetHostErrors(),
			gotErrs.GetHostErrors(), defResCmpOpts()...); diff != "" {

			t.Fatalf("unexpected host errors (-want, +got):\n%s\n", diff)
		}
		return
	}

	if gotErrs != nil {
		t.Fatalf("unexpected host errors %s", gotErrs.Errors())
	}
}

func TestControl_AutoConfig_getNetworkSet(t *testing.T) {
	for name, tc := range map[string]struct {
		uErr          error
		hostResponses []*HostResponse
		expErr        error
		expHostErrs   []*MockHostError
		expNetSet     *HostFabricSet
	}{
		"invoker error": {
			uErr:          errors.New("unary error"),
			hostResponses: dualHostRespSame(fabIfs1),
			expErr:        errors.New("unary error"),
		},
		"host network scan failed": {
			hostResponses: hostRespRemoteFail,
			expHostErrs: []*MockHostError{
				{"host2", "remote failed"},
				{"host2", "remote failed"},
			},
			expErr: errors.New("1 host had errors"),
		},
		"host network scan failed on multiple hosts": {
			hostResponses: hostRespRemoteFails,
			expHostErrs: []*MockHostError{
				{"host1", "remote failed"},
				{"host2", "remote failed"},
			},
			expErr: errors.New("2 hosts had errors"),
		},
		"host network scan no hosts": {
			hostResponses: []*HostResponse{},
			expErr:        errors.New("no host responses"),
		},
		"host network mismatch": {
			hostResponses: dualHostResp(fabIfs1, fabIfs2),
			expErr:        errors.New("network hardware not consistent across hosts"),
		},
		"zero numa on single host": {
			hostResponses: dualHostResp(fabIfs1, fabIfs1wNuma),
			expErr:        errors.New("network hardware not consistent across hosts"),
		},
		"success": {
			hostResponses: dualHostRespSame(fabIfs1),
			expNetSet: &HostFabricSet{
				HostSet: hostlist.MustCreateSet("host[1-2]"),
				HostFabric: &HostFabric{
					Interfaces: []*HostFabricInterface{
						{Provider: "test-provider", Device: "test-device", NumaNode: 42},
						{Provider: "test-provider", Device: "test-device2", NumaNode: 84},
					},
					Providers: []string{"test-provider"},
				},
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			netSet, err := fabricFromHostResp(t, log, tc.uErr, tc.hostResponses)
			test.CmpErr(t, tc.expErr, err)

			// Additionally verify any internal error details.
			var gotHostErrs *HostErrorsResp
			if cge, ok := err.(*ConfGenerateError); ok {
				gotHostErrs = &cge.HostErrorsResp
			}
			cmpHostErrs(t, tc.expHostErrs, gotHostErrs)
			if tc.expErr != nil {
				return
			}
			if tc.expHostErrs != nil || gotHostErrs != nil {
				t.Fatal("expected or received host errors without outer error")
			}

			cmpOpts := []cmp.Option{
				cmpopts.IgnoreUnexported(
					HostFabricSet{},
				),
			}
			cmpOpts = append(cmpOpts, defResCmpOpts()...)

			if diff := cmp.Diff(tc.expNetSet, netSet, cmpOpts...); diff != "" {
				t.Fatalf("unexpected network set (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func TestControl_AutoConfig_getNetworkDetails(t *testing.T) {
	ib0PB := new(ctlpb.FabricInterface)
	if err := convert.Types(ib0, ib0PB); err != nil {
		t.Fatal(err)
	}
	eth0PB := new(ctlpb.FabricInterface)
	if err := convert.Types(eth0, eth0PB); err != nil {
		t.Fatal(err)
	}
	fabIfs3 := &ctlpb.NetworkScanResp{
		Interfaces: []*ctlpb.FabricInterface{ib0PB, eth0PB}, Numacount: 2, Corespernuma: 24,
	}
	typicalFabIfs := &ctlpb.NetworkScanResp{Interfaces: typIfs, Numacount: 2, Corespernuma: 24}

	for name, tc := range map[string]struct {
		netDevClass   hardware.NetDevClass
		netProvider   string
		hostResponses []*HostResponse
		expErr        error
		expNetDetails networkDetails
	}{
		"unsupported network class in request": {
			netDevClass:   2,
			hostResponses: dualHostRespSame(fabIfs1wNuma),
			expErr:        errors.New("unsupported net dev class in request"),
		},
		"single numa": {
			hostResponses: dualHostRespSame(fabIfs3),
			expNetDetails: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: ib0},
				},
				NumaCoreCount: 24,
				NumaCount:     2,
			},
		},
		"single numa; select infiniband": {
			netDevClass:   hardware.Infiniband,
			hostResponses: dualHostRespSame(fabIfs3),
			expNetDetails: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: ib0},
				},
				NumaCoreCount: 24,
				NumaCount:     2,
			},
		},
		"single numa; select ethernet": {
			netDevClass:   hardware.Ether,
			hostResponses: dualHostRespSame(fabIfs3),
			expNetDetails: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+tcp": {0: eth0},
				},
				NumaCoreCount: 24,
				NumaCount:     2,
			},
		},
		"dual numa with typical fabric scan output": {
			hostResponses: dualHostRespSame(typicalFabIfs),
			expNetDetails: networkDetails{
				ProviderIfaces: pbIfs2ProvMap(t, typIfs, hardware.Infiniband),
				NumaCoreCount:  24,
				NumaCount:      2,
			},
		},
		"dual numa with typical fabric scan output; ethernet": {
			netDevClass:   hardware.Ether,
			hostResponses: dualHostRespSame(typicalFabIfs),
			expNetDetails: networkDetails{
				ProviderIfaces: pbIfs2ProvMap(t, typIfs, hardware.Ether),
				NumaCoreCount:  24,
				NumaCount:      2,
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			if tc.netDevClass == 0 {
				tc.netDevClass = hardware.Infiniband
			}

			netSet, err := fabricFromHostResp(t, log, nil, tc.hostResponses)
			if err != nil {
				t.Fatal(err)
			}

			gotNetDetails, gotErr := getNetworkDetails(log, tc.netDevClass, tc.netProvider, netSet.HostFabric)
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			// Manually update interface provider based on map key to eliminate need for
			// extra fixtures.
			expProvIfs := tc.expNetDetails.ProviderIfaces
			for prov, numaIfaces := range expProvIfs {
				for nn := range numaIfaces {
					expProvIfs[prov][nn].Provider = prov
				}
			}

			if diff := cmp.Diff(tc.expNetDetails, *gotNetDetails); diff != "" {
				t.Fatalf("unexpected provider interface map (-want, +got):\n%s\n", diff)
			}
		})
	}
}

type mockHostResponses struct {
	resps     []*HostResponse
	numaSSDs  map[uint32][]string
	numaPMEMs map[uint32][]string
}

func newMockHostResponses(t *testing.T, variants ...string) *mockHostResponses {
	t.Helper()

	var inResp *ctlpb.StorageScanResp
	var outResps *mockHostResponses

	switch len(variants) {
	case 0:
		t.Fatal("no host response variants")
	case 1:
		inResp = MockServerScanResp(t, variants[0])

		outResps = &mockHostResponses{
			resps: []*HostResponse{
				{
					Addr:    "host1",
					Message: inResp,
				},
				{
					Addr:    "host2",
					Message: inResp,
				},
			},
		}
	case 2:
		inResp = MockServerScanResp(t, variants[0])

		outResps = &mockHostResponses{
			resps: []*HostResponse{
				{
					Addr:    "host1",
					Message: MockServerScanResp(t, variants[0]),
				},
				{
					Addr:    "host2",
					Message: MockServerScanResp(t, variants[1]),
				},
			},
		}
	default:
		t.Fatal("no host response variants")
	}

	pmems := make(map[uint32][]string)
	for _, p := range inResp.Scm.Namespaces {
		pmems[p.NumaNode] = append(pmems[p.NumaNode],
			fmt.Sprintf("%s/%s", scmBdevDir, p.Blockdev))
		sort.Strings(pmems[p.NumaNode])
	}
	outResps.numaPMEMs = pmems

	ssds := make(map[uint32][]string)
	for _, c := range inResp.Nvme.Ctrlrs {
		ssds[uint32(c.SocketId)] = append(ssds[uint32(c.SocketId)], c.PciAddr)
		sort.Strings(ssds[uint32(c.SocketId)])
	}
	outResps.numaSSDs = ssds

	return outResps
}

func (mhr *mockHostResponses) getNUMASSDs(t *testing.T, numa uint32) []string {
	t.Helper()

	addrs, exists := mhr.numaSSDs[numa]
	if !exists {
		t.Fatalf("no ssds for numa %d", numa)
	}

	return addrs
}

func (mhr *mockHostResponses) getNUMAPMEMs(t *testing.T, numa uint32) []string {
	t.Helper()

	pmems, exists := mhr.numaPMEMs[numa]
	if !exists {
		t.Fatalf("no pmems for numa %d", numa)
	}

	return pmems
}

func TestControl_AutoConfig_getStorageSet(t *testing.T) {
	oneWithScmNs := newMockHostResponses(t, "pmemSingle", "standard")
	oneScanFail := newMockHostResponses(t, "standard", "bothFailed")
	scanFail := newMockHostResponses(t, "bothFailed")
	withScmNs := newMockHostResponses(t, "pmemSingle")
	diffHpSizes := newMockHostResponses(t, "withSpaceUsage", "1gbHugepages")

	for name, tc := range map[string]struct {
		uErr          error
		hostResponses []*HostResponse
		expErr        error
		expHostErrs   []*MockHostError
		expStorageSet *HostStorageSet
	}{
		"invoker error": {
			uErr:          errors.New("unary error"),
			hostResponses: oneWithScmNs.resps,
			expErr:        errors.New("unary error"),
		},
		"host storage scan; failed": {
			hostResponses: oneScanFail.resps,
			expHostErrs: []*MockHostError{
				{"host2", "scm scan failed"},
				{"host2", "nvme scan failed"},
			},
			expErr: errors.New("1 host had errors"),
		},
		"host storage scan; failed on multiple hosts": {
			hostResponses: scanFail.resps,
			expHostErrs: []*MockHostError{
				{"host1", "scm scan failed"},
				{"host1", "nvme scan failed"},
				{"host2", "scm scan failed"},
				{"host2", "nvme scan failed"},
			},
			expErr: errors.New("2 hosts had errors"),
		},
		"host storage scan; no hosts": {
			hostResponses: []*HostResponse{},
			expErr:        errors.New("no host responses"),
		},
		"host storage scan; mismatch": {
			hostResponses: oneWithScmNs.resps,
			expErr:        errors.New("storage hardware not consistent across hosts"),
		},
		"diff hugepage sizes": {
			hostResponses: diffHpSizes.resps,
			expErr:        errors.New("not consistent"),
		},
		"success": {
			hostResponses: withScmNs.resps,
			expStorageSet: &HostStorageSet{
				HostSet: hostlist.MustCreateSet("host[1-2]"),
				HostStorage: &HostStorage{
					NvmeDevices:   storage.NvmeControllers{storage.MockNvmeController()},
					ScmModules:    storage.ScmModules{storage.MockScmModule()},
					ScmNamespaces: storage.ScmNamespaces{storage.MockScmNamespace(0)},
					HugePageInfo:  HugePageInfo{PageSizeKb: humanize.KiByte * 2},
				},
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			mi := NewMockInvoker(log, &MockInvokerConfig{
				UnaryError: tc.uErr,
				UnaryResponse: &UnaryResponse{
					Responses: tc.hostResponses,
				},
			})

			storageSet, err := getStorageSet(context.TODO(), log, []string{}, mi)
			test.CmpErr(t, tc.expErr, err)

			// Additionally verify any internal error details.
			var gotHostErrs *HostErrorsResp
			if cge, ok := err.(*ConfGenerateError); ok {
				gotHostErrs = &cge.HostErrorsResp
			}
			cmpHostErrs(t, tc.expHostErrs, gotHostErrs)
			if tc.expErr != nil {
				return
			}
			if tc.expHostErrs != nil || gotHostErrs != nil {
				t.Fatal("expected or received host errors without outer error")
			}

			cmpOpts := append([]cmp.Option{
				cmpopts.IgnoreUnexported(HostStorageSet{}),
				cmpopts.IgnoreFields(storage.NvmeController{}, "Serial"),
			}, defResCmpOpts()...)

			if diff := cmp.Diff(tc.expStorageSet, storageSet, cmpOpts...); diff != "" {
				t.Fatalf("unexpected network set (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func TestControl_AutoConfig_getStorageDetails(t *testing.T) {
	withSSDs := newMockHostResponses(t, "withSpaceUsage")
	withSSDsBadPCI := newMockHostResponses(t, "badPciAddr")
	noSSDsOnNUMA1 := newMockHostResponses(t, "noNvmeOnNuma1")

	for name, tc := range map[string]struct {
		hostResponses []*HostResponse
		expErr        error
		expPMems      [][]string
		expSSDs       [][]string
	}{
		"bad ssd pci address": {
			hostResponses: withSSDsBadPCI.resps,
			expErr:        errors.New("unexpected pci address"),
		},
		"2 numa nodes; 2 ssds": {
			hostResponses: withSSDs.resps,
			expPMems: [][]string{
				withSSDs.getNUMAPMEMs(t, 0),
				withSSDs.getNUMAPMEMs(t, 1),
			},
			expSSDs: [][]string{
				withSSDs.getNUMASSDs(t, 0),
				withSSDs.getNUMASSDs(t, 1),
			},
		},
		"2 numa nodes; 0 ssds on numa 1": {
			hostResponses: noSSDsOnNUMA1.resps,
			expPMems: [][]string{
				noSSDsOnNUMA1.getNUMAPMEMs(t, 0),
				noSSDsOnNUMA1.getNUMAPMEMs(t, 1),
			},
			expSSDs: [][]string{
				noSSDsOnNUMA1.getNUMASSDs(t, 0),
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			mi := NewMockInvoker(log, &MockInvokerConfig{
				UnaryResponse: &UnaryResponse{
					Responses: tc.hostResponses,
				},
			})

			storageSet, err := getStorageSet(context.TODO(), log, []string{}, mi)
			if err != nil {
				t.Fatal(err)
			}

			gotStorage, gotErr := getStorageDetails(log, storageSet.HostStorage)
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			test.AssertEqual(t, len(tc.expPMems), len(gotStorage.NumaSCMs),
				"unexpected number of pmem devices")
			for nn, pmems := range gotStorage.NumaSCMs {
				if diff := cmp.Diff(tc.expPMems[nn], []string(pmems)); diff != "" {
					t.Fatalf("unexpected pmem paths (-want, +got):\n%s\n", diff)
				}
			}

			test.AssertEqual(t, len(tc.expSSDs), len(gotStorage.NumaSSDs),
				"unexpected number of ssds")
			for nn, ssds := range gotStorage.NumaSSDs {
				if diff := cmp.Diff(tc.expSSDs[nn], ssds.Strings()); diff != "" {
					t.Fatalf("unexpected list of ssds (-want, +got):\n%s\n", diff)
				}
			}
		})
	}
}

func TestControl_AutoConfig_filterDevicesByAffinity(t *testing.T) {
	singlePMemMap := numaSCMsMap{0: []string{"/dev/pmem0"}}

	for name, tc := range map[string]struct {
		nrEngines  int
		minNrSSDs  int
		sd         storageDetails
		nd         networkDetails
		expErr     error
		expNumaSet []int          // set of numa nodes (by-ID) to be used for engine configs
		expSD      storageDetails // expected details after updates
		expND      networkDetails
	}{
		"nr engines unset; zero numa count": {
			expErr: errNoNuma,
		},
		"nr engines unset; missing scm": {
			nd: networkDetails{
				NumaCount: 2,
			},
			expErr: errors.Errorf(errInsufNrPMemGroups, numaSCMsMap(nil), 2, 0),
		},
		"nr engines set; insufficient scm": {
			nd: networkDetails{
				NumaCount: 2,
			},
			sd: storageDetails{
				NumaSCMs: singlePMemMap,
			},
			expErr: errors.Errorf(errInsufNrPMemGroups, singlePMemMap, 2, 1),
		},
		"missing ssds; zero min nr; no fabric": {
			nrEngines: 1,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
			expErr: errors.New(errInsufNrProvGroups),
		},
		"missing ssds; zero min nr; no matching fabric": {
			nrEngines: 1,
			sd: storageDetails{
				NumaSCMs: singlePMemMap,
			},
			nd: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {1: ib1},
					"ofi+tcp":  {1: ib1},
				},
			},
			expErr: errors.New(errInsufNrProvGroups),
		},
		"missing ssds; zero min nr; matching fabric": {
			nrEngines: 1,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
			nd: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {1: ib1},
					"ofi+tcp":  {1: ib1},
				},
			},
			expNumaSet: []int{1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{1: {}},
			},
			expND: networkDetails{
				NumaIfaces: numaNetIfaceMap{1: ib1},
			},
		},
		"missing ssds; 1 min nr": {
			nrEngines: 1,
			minNrSSDs: 1,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
			expErr: errors.Errorf(errInsufNrSSDs, 0, 1, 0),
		},
		"insufficient ssds; 3 min nr; numa 1 has 2": {
			nrEngines: 2,
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4)...),
				},
			},
			expErr: errors.Errorf(errInsufNrSSDs, 1, 3, 2),
		},
		"sufficient ssds; 3 min nr; matching fabric": {
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				NumaCount: 2,
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: ib0, 1: ib1},
					"ofi+tcp":  {0: ib0, 1: ib1},
				},
			},
			expNumaSet: []int{0, 1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			expND: networkDetails{
				NumaCount:  2,
				NumaIfaces: numaNetIfaceMap{0: ib0, 1: ib1},
			},
		},
		"sufficient ssds; 3 min nr; matching ether fabric": {
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				NumaCount: 2,
				ProviderIfaces: providerIfaceMap{
					"ofi+tcp": {0: eth0, 1: eth1},
				},
			},
			expNumaSet: []int{0, 1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			expND: networkDetails{
				NumaCount:  2,
				NumaIfaces: numaNetIfaceMap{0: eth0, 1: eth1},
			},
		},
		"sufficient ssds; 3 min nr; matching fabric on one numa only": {
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				NumaCount: 2,
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: ib0},
					"ofi+tcp":  {0: ib0},
				},
			},
			expErr: errors.New(errInsufNrProvGroups),
		},
		"sufficient ssds; 3 min nr; matching ethernet fabric": {
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				NumaCount: 2,
				ProviderIfaces: providerIfaceMap{
					"ofi+sockets": {0: &HostFabricInterface{
						Provider:    "ofi+sockets",
						Device:      "eth2",
						NumaNode:    0,
						NetDevClass: 32,
						Priority:    50,
					}},
					"ofi+tcp": {0: eth0, 1: eth1},
				},
			},
			expNumaSet: []int{0, 1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			expND: networkDetails{
				NumaCount:  2,
				NumaIfaces: numaNetIfaceMap{0: eth0, 1: eth1},
			},
		},
		"single engine requested; both numa match criteria; select max nr ssds": {
			nrEngines: 1,
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: &HostFabricInterface{
						Provider:    "ofi+psm2",
						Device:      "ib2",
						NumaNode:    1,
						NetDevClass: 32,
						Priority:    1,
					}, 1: ib1},
				},
			},
			expNumaSet: []int{1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			expND: networkDetails{
				NumaIfaces: numaNetIfaceMap{1: ib1},
			},
		},
		"single engine requested; both numa match criteria; select best fabric": {
			nrEngines: 1,
			minNrSSDs: 3,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5, 6)...),
				},
			},
			nd: networkDetails{
				ProviderIfaces: providerIfaceMap{
					"ofi+psm2": {0: ib0, 1: ib1},
				},
			},
			expNumaSet: []int{0},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
				},
			},
			expND: networkDetails{
				NumaIfaces: numaNetIfaceMap{0: ib0},
			},
		},
		"both numa match criteria; typical fabric scan output": {
			minNrSSDs: 1,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4)...),
				},
			},
			nd: networkDetails{
				NumaCount:      2,
				ProviderIfaces: pbIfs2ProvMap(t, typIfs, hardware.Infiniband),
			},
			expNumaSet: []int{0, 1},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4)...),
				},
			},
			expND: networkDetails{
				NumaCount: 2,
				NumaIfaces: numaNetIfaceMap{
					0: &HostFabricInterface{
						Provider:    "ofi+psm2",
						Device:      "ib0",
						NumaNode:    0,
						NetDevClass: 32,
						Priority:    1,
					},
					1: &HostFabricInterface{
						Provider:    "ofi+psm2",
						Device:      "ib1",
						NumaNode:    1,
						NetDevClass: 32,
						Priority:    0,
					},
				},
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			gotNumaSet, gotErr := filterDevicesByAffinity(log, tc.nrEngines,
				tc.minNrSSDs, &tc.nd, &tc.sd)
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expNumaSet, gotNumaSet); diff != "" {
				t.Fatalf("unexpected numa set selected (-want, +got):\n%s\n", diff)
			}
			if diff := cmp.Diff(tc.expND, tc.nd); diff != "" {
				t.Fatalf("unexpected network details (-want, +got):\n%s\n", diff)
			}
			if diff := cmp.Diff(tc.expSD, tc.sd, pciSetCmpOpts...); diff != "" {
				t.Fatalf("unexpected storage details (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func TestControl_AutoConfig_correctSSDCounts(t *testing.T) {
	for name, tc := range map[string]struct {
		sd           storageDetails
		nvmeDisabled bool
		expErr       error
		expSD        storageDetails // expected details after updates
	}{
		"no ssds": {
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
			expErr: errors.New("could not be calculated"),
		},
		"no ssds; nvme disabled": {
			nvmeDisabled: true,
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
			},
		},
		"adjust ssd count to global minimum": {
			sd: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4)...),
				},
			},
			expSD: storageDetails{
				NumaSCMs: numaSCMsMap{
					0: []string{"/dev/pmem0"},
					1: []string{"/dev/pmem1"},
				},
				NumaSSDs: numaSSDsMap{
					0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1)...),
					1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4)...),
				},
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			minNrSSDs := 1
			if tc.nvmeDisabled {
				minNrSSDs = 0
			}

			gotErr := correctSSDCounts(log, minNrSSDs, &tc.sd)
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expSD, tc.sd, pciSetCmpOpts...); diff != "" {
				t.Fatalf("unexpected storage details (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func mockEngineCfg(idx int) *engine.Config {
	return engine.MockConfig().
		WithTargetCount(defaultTargetCount).
		WithLogFile(fmt.Sprintf("%s.%d.log", defaultEngineLogFile, idx))
}

func TestControl_AutoConfig_genEngineConfigs(t *testing.T) {
	for name, tc := range map[string]struct {
		minNrSSDs  int
		numaSet    []int            // set of numa nodes (by-ID) to be used for engine configs
		numaPMems  numaSCMsMap      // numa to pmem mappings
		numaSSDs   numaSSDsMap      // numa to ssds mappings
		numaIfaces numaNetIfaceMap  // numa to network interface mappings
		expCfgs    []*engine.Config // expected generated engine configs
		expErr     error
	}{
		"missing scm": {
			numaSet:    []int{0},
			numaSSDs:   numaSSDsMap{0: hardware.MustNewPCIAddressSet()},
			numaIfaces: numaNetIfaceMap{0: ib0},
			expErr:     errors.New("no scm device"),
		},
		"missing ssds": {
			numaSet:    []int{0},
			numaPMems:  numaSCMsMap{0: []string{"/dev/pmem0"}},
			numaIfaces: numaNetIfaceMap{0: ib0},
			expErr:     errors.New("no ssds"),
		},
		"missing fabric": {
			numaSet:   []int{0},
			numaPMems: numaSCMsMap{0: []string{"/dev/pmem0"}},
			numaSSDs:  numaSSDsMap{0: hardware.MustNewPCIAddressSet()},
			expErr:    errors.New("no fabric"),
		},
		"missing scm on second numa": {
			numaSet:   []int{0, 1},
			numaPMems: numaSCMsMap{0: []string{"/dev/pmem0"}},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet(),
				1: hardware.MustNewPCIAddressSet(),
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
				1: ib1,
			},
			expErr: errors.New("no scm device found for numa 1"),
		},
		"missing ssds on second numa": {
			numaSet: []int{0, 1},
			numaPMems: numaSCMsMap{
				0: []string{"/dev/pmem0"},
				1: []string{"/dev/pmem1"},
			},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet(),
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
				1: ib1,
			},
			expErr: errors.New("no ssds found for numa 1"),
		},
		"missing fabric on second numa": {
			numaSet: []int{0, 1},
			numaPMems: numaSCMsMap{
				0: []string{"/dev/pmem0"},
				1: []string{"/dev/pmem1"},
			},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet(),
				1: hardware.MustNewPCIAddressSet(),
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
			},
			expErr: errors.New("no fabric interface found for numa 1"),
		},
		"single pmem zero ssds; missing numa set": {
			numaPMems:  numaSCMsMap{0: []string{"/dev/pmem0"}},
			numaSSDs:   numaSSDsMap{0: hardware.MustNewPCIAddressSet()},
			numaIfaces: numaNetIfaceMap{0: ib0},
			expCfgs:    []*engine.Config{},
		},
		"single pmem zero ssds": {
			numaSet:    []int{0},
			numaPMems:  numaSCMsMap{0: []string{"/dev/pmem0"}},
			numaSSDs:   numaSSDsMap{0: hardware.MustNewPCIAddressSet()},
			numaIfaces: numaNetIfaceMap{0: ib0},
			expCfgs: []*engine.Config{
				DefaultEngineCfg(0).
					WithPinnedNumaNode(0).
					WithFabricInterface("ib0").
					WithFabricInterfacePort(defaultFiPort).
					WithFabricProvider("ofi+psm2").
					WithStorage(
						storage.NewTierConfig().
							WithNumaNodeIndex(0).
							WithStorageClass(storage.ClassDcpm.String()).
							WithScmDeviceList("/dev/pmem0").
							WithScmMountPoint("/mnt/daos0"),
					).
					WithStorageConfigOutputPath("").
					WithHelperStreamCount(2),
			},
		},
		"dual pmem dual ssd": {
			numaSet: []int{0, 1},
			// ndctl doesn't guarantee that pmem0 will be created on numa0
			numaPMems: numaSCMsMap{
				0: []string{"/dev/pmem1"},
				1: []string{"/dev/pmem0"},
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
				1: ib1,
			},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(0, 1, 2)...),
				1: hardware.MustNewPCIAddressSet(test.MockPCIAddrs(3, 4, 5)...),
			},
			expCfgs: []*engine.Config{
				DefaultEngineCfg(0).
					WithPinnedNumaNode(0).
					WithFabricInterface("ib0").
					WithFabricInterfacePort(defaultFiPort).
					WithFabricProvider("ofi+psm2").
					WithStorage(
						storage.NewTierConfig().
							WithNumaNodeIndex(0).
							WithStorageClass(storage.ClassDcpm.String()).
							WithScmDeviceList("/dev/pmem1").
							WithScmMountPoint("/mnt/daos0"),
						storage.NewTierConfig().
							WithNumaNodeIndex(0).
							WithStorageClass(storage.ClassNvme.String()).
							WithBdevDeviceList(test.MockPCIAddrs(0, 1, 2)...),
					).
					WithTargetCount(16).
					WithHelperStreamCount(2),
				DefaultEngineCfg(1).
					WithPinnedNumaNode(1).
					WithFabricInterface("ib1").
					WithFabricInterfacePort(int(defaultFiPort+defaultFiPortInterval)).
					WithFabricProvider("ofi+psm2").
					WithFabricNumaNodeIndex(1).
					WithStorage(
						storage.NewTierConfig().
							WithNumaNodeIndex(1).
							WithStorageClass(storage.ClassDcpm.String()).
							WithScmDeviceList("/dev/pmem0").
							WithScmMountPoint("/mnt/daos1"),
						storage.NewTierConfig().
							WithNumaNodeIndex(1).
							WithStorageClass(storage.ClassNvme.String()).
							WithBdevDeviceList(test.MockPCIAddrs(3, 4, 5)...),
					).
					WithStorageNumaNodeIndex(1).
					WithTargetCount(16).
					WithHelperStreamCount(2),
			},
		},
		"vmd enabled; balanced nr ssds": {
			numaSet: []int{0, 1},
			numaPMems: numaSCMsMap{
				0: []string{"/dev/pmem0"},
				1: []string{"/dev/pmem1"},
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
				1: ib1,
			},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet("5d0505:01:00.0", "5d0505:02:00.0"),
				1: hardware.MustNewPCIAddressSet("d70701:03:00.0", "d70701:05:00.0"),
			},
			expCfgs: []*engine.Config{
				DefaultEngineCfg(0).
					WithPinnedNumaNode(0).
					WithFabricInterface("ib0").
					WithFabricInterfacePort(defaultFiPort).
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
							WithBdevDeviceList("0000:5d:05.5"),
					).
					WithTargetCount(16).
					WithHelperStreamCount(2),
				DefaultEngineCfg(1).
					WithPinnedNumaNode(1).
					WithFabricInterface("ib1").
					WithFabricInterfacePort(
						int(defaultFiPort+defaultFiPortInterval)).
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
							WithBdevDeviceList("0000:d7:07.1"),
					).
					WithStorageNumaNodeIndex(1).
					WithTargetCount(16).
					WithHelperStreamCount(2),
			},
		},
		"vmd enabled; imbalanced nr ssds": {
			numaSet: []int{0, 1},
			numaPMems: numaSCMsMap{
				0: []string{"/dev/pmem0"},
				1: []string{"/dev/pmem1"},
			},
			numaIfaces: numaNetIfaceMap{
				0: ib0,
				1: ib1,
			},
			numaSSDs: numaSSDsMap{
				0: hardware.MustNewPCIAddressSet(test.MockVMDPCIAddrs(5, 2, 4)...),
				1: hardware.MustNewPCIAddressSet(test.MockVMDPCIAddrs(13, 1, 2, 3)...),
			},
			expErr: FaultConfigVMDImbalance,
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			nd := &networkDetails{
				NumaIfaces: tc.numaIfaces,
			}
			sd := &storageDetails{
				HugePageSize: 2048,
				NumaSCMs:     tc.numaPMems,
				NumaSSDs:     tc.numaSSDs,
			}
			if tc.minNrSSDs == 0 {
				tc.minNrSSDs = 1
			}

			gotCfgs, gotErr := genEngineConfigs(log, tc.minNrSSDs, mockEngineCfg,
				tc.numaSet, nd, sd)
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
			}
			cmpOpts = append(cmpOpts, defResCmpOpts()...)

			if diff := cmp.Diff(tc.expCfgs, gotCfgs, cmpOpts...); diff != "" {
				t.Fatalf("unexpected engine configs (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func TestControl_AutoConfig_getThreadCounts(t *testing.T) {
	defaultScmTier := storage.NewTierConfig().
		WithStorageClass(storage.ClassDcpm.String()).
		WithScmMountPoint("/mnt/test0").
		WithScmDeviceList("/dev/pmem0")

	for name, tc := range map[string]struct {
		numaCoreCount int // physical cores per NUMA node
		nrSSDs        int32
		expNrTgts     int
		expNrHlprs    int
		expErr        error
	}{
		"no cores":         {0, 0, 0, 0, errors.Errorf(errInvalNrCores, 0)},
		"simplest case":    {5, 1, 4, 1, nil},
		"24 cores no ssds": {24, 0, 16, 0, nil},
		"24 cores 1 ssds":  {24, 1, 19, 4, nil},
		"24 cores 2 ssds":  {24, 2, 18, 4, nil},
		"24 cores 3 ssds":  {24, 3, 18, 4, nil},
		"24 cores 4 ssds":  {24, 4, 16, 4, nil},
		"24 cores 5 ssds":  {24, 5, 15, 3, nil},
		"24 cores 8 ssds":  {24, 8, 16, 4, nil},
		"24 cores 9 ssds":  {24, 9, 18, 4, nil},
		"24 cores 10 ssds": {24, 10, 10, 2, nil},
		"24 cores 16 ssds": {24, 16, 16, 4, nil},
		"18 cores no ssds": {18, 0, 16, 0, nil},
		"18 cores 1 ssds":  {18, 1, 14, 3, nil},
		"18 cores 2 ssds":  {18, 2, 14, 3, nil},
		"18 cores 3 ssds":  {18, 3, 12, 3, nil},
		"18 cores 4 ssds":  {18, 4, 12, 3, nil},
		"18 cores 5 ssds":  {18, 5, 10, 2, nil},
		"18 cores 8 ssds":  {18, 8, 8, 2, nil},
		"18 cores 9 ssds":  {18, 9, 9, 2, nil},
		"18 cores 10 ssds": {18, 10, 10, 2, nil},
		"18 cores 16 ssds": {18, 16, 16, 0, nil},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			var addrs []string
			for i := int32(0); i < tc.nrSSDs; i++ {
				addrs = append(addrs, test.MockPCIAddr(i))
			}

			// TODO DAOS-11859: Test calculation based on MD-on-SSD (bdev tiers)
			tiers := storage.TierConfigs{
				defaultScmTier,
				storage.NewTierConfig().
					WithStorageClass(storage.ClassNvme.String()).
					WithBdevDeviceList(addrs...),
			}
			cfg := mockEngineCfg(0).WithStorage(tiers...)

			gotCounts, gotErr := getThreadCounts(log, cfg, tc.numaCoreCount)
			test.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expNrTgts, gotCounts.nrTgts); diff != "" {
				t.Fatalf("unexpected target counts (-want, +got):\n%s\n", diff)
			}
			if diff := cmp.Diff(tc.expNrHlprs, gotCounts.nrHlprs); diff != "" {
				t.Fatalf("unexpected helper counts (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func TestControl_AutoConfig_genConfig(t *testing.T) {
	defHpSizeKb := 2048
	exmplEngineCfg := DefaultEngineCfg(0).
		WithPinnedNumaNode(0).
		WithFabricInterface("ib0").
		WithFabricInterfacePort(defaultFiPort).
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
				WithBdevDeviceList(test.MockPCIAddrs(0, 1, 2)...),
		).
		WithTargetCount(16).
		WithHelperStreamCount(2)
	exmplEngineCfgs := []*engine.Config{
		exmplEngineCfg,
		DefaultEngineCfg(1).
			WithPinnedNumaNode(1).
			WithFabricInterface("ib1").
			WithFabricInterfacePort(int(defaultFiPort+defaultFiPortInterval)).
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
					WithBdevDeviceList(test.MockPCIAddrs(3, 4, 5)...),
			).
			WithStorageNumaNodeIndex(1).
			WithTargetCount(16).
			WithHelperStreamCount(2),
	}
	baseConfig := func(prov string, ecs []*engine.Config) *config.Server {
		for idx, ec := range ecs {
			ec.WithStorageConfigOutputPath(fmt.Sprintf("/mnt/daos%d/daos_nvme.conf", idx)).
				WithStorageVosEnv("NVME")
		}
		return config.DefaultServer().
			WithControlLogFile(defaultControlLogFile).
			WithFabricProvider(prov).
			WithDisableVMD(false).
			WithEngines(ecs...)
	}

	for name, tc := range map[string]struct {
		accessPoints []string // list of access point host/ip addresses
		ecs          []*engine.Config
		hpSize       int
		threadCounts *threadCounts  // numa to cpu mappings
		expCfg       *config.Server // expected config generated
		expErr       error
	}{
		"no engines": {
			expErr: errors.New("numTargets must be > 0"),
		},
		"no hugepage size": {
			threadCounts: &threadCounts{16, 0},
			ecs:          []*engine.Config{mockEngineCfg(0)},
			expErr:       errors.New("invalid system hugepage size"),
		},
		"no provider in engine config": {
			threadCounts: &threadCounts{16, 0},
			ecs:          []*engine.Config{mockEngineCfg(0)},
			hpSize:       defHpSizeKb,
			expErr:       errors.New("provider not specified"),
		},
		"single engine config": {
			threadCounts: &threadCounts{16, 0},
			ecs:          []*engine.Config{exmplEngineCfg},
			hpSize:       defHpSizeKb,
			expCfg: baseConfig(exmplEngineCfg.Fabric.Provider,
				[]*engine.Config{exmplEngineCfg}).
				WithNrHugePages(8192).
				WithAccessPoints("hostX:10002"),
		},
		"dual engine config": {
			threadCounts: &threadCounts{16, 0},
			ecs:          exmplEngineCfgs,
			hpSize:       defHpSizeKb,
			expCfg: baseConfig(exmplEngineCfg.Fabric.Provider,
				exmplEngineCfgs).
				WithNrHugePages(16384).
				WithAccessPoints("hostX:10002"),
		},
		"bad accesspoint port": {
			accessPoints: []string{"hostX:-10001"},
			threadCounts: &threadCounts{16, 0},
			ecs:          exmplEngineCfgs,
			hpSize:       defHpSizeKb,
			expErr:       config.FaultConfigBadControlPort,
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer test.ShowBufferOnFailure(t, buf)

			if tc.accessPoints == nil {
				tc.accessPoints = []string{"hostX:10002"}
			}
			if tc.threadCounts == nil {
				tc.threadCounts = &threadCounts{}
			}

			gotResp, gotErr := genServerConfig(log, tc.accessPoints, tc.ecs, tc.hpSize,
				tc.threadCounts)
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
			cmpOpts = append(cmpOpts, defResCmpOpts()...)

			if diff := cmp.Diff(tc.expCfg, gotResp, cmpOpts...); diff != "" {
				t.Fatalf("unexpected engine configs (-want, +got):\n%s\n", diff)
			}
		})
	}
}
