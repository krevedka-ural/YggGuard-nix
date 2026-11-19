package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func validProfile() Profile {
	return Profile{Host: "300::42", Port: 1080, Peers: []string{"tls://peer.example:443"}, PrivateKey: strings.Repeat("ab", 64)}
}
func TestRejectInvalidProfile(t *testing.T) {
	cases := []Profile{}
	for _, host := range []string{"127.0.0.1", "::1", "2001:db8::1", "300::1%eth0", "300::1\nListen ::"} {
		p := validProfile()
		p.Host = host
		cases = append(cases, p)
	}
	p := validProfile()
	p.Port = 0
	cases = append(cases, p)
	p = validProfile()
	p.PrivateKey = "bad"
	cases = append(cases, p)
	p = validProfile()
	p.Peers = nil
	cases = append(cases, p)
	for _, peer := range []string{"file:///etc/shadow", "tcp://peer.example", "tcp://root:pw@peer:443", "tcp://peer:70000"} {
		p := validProfile()
		p.Peers = []string{peer}
		cases = append(cases, p)
	}
	for _, p := range cases {
		if err := p.validate(); err == nil {
			t.Errorf("accepted invalid profile: host %q port %d peers %v", p.Host, p.Port, p.Peers)
		}
	}
	if err := validProfile().validate(); err != nil {
		t.Fatal(err)
	}
}
func TestTunnelHasNoDirectFallbackAndDnsGoesThroughProxy(t *testing.T) {
	config := tunConfig(validProfile())
	out := config["outbounds"].([]any)
	if len(out) != 1 || out[0].(map[string]any)["type"] != "http" {
		t.Fatal("unexpected outbound/fallback", out)
	}
	dns := config["dns"].(map[string]any)["servers"].([]any)[0].(map[string]any)
	if dns["type"] != "https" || dns["detour"] != "proxy" || dns["server_port"] != 443 {
		t.Fatal("DNS may bypass proxy", dns)
	}
	in := config["inbounds"].([]any)[0].(map[string]any)
	if !reflect.DeepEqual(in["exclude_uid"], []int{0}) {
		t.Fatal("root overlay sockets must bypass TUN")
	}
	if config["route"].(map[string]any)["final"] != "proxy" {
		t.Fatal("final must use proxy")
	}
	ygg := yggConfig(validProfile())
	if ygg["AdminListen"] != "none" {
		t.Fatal("admin socket exposed")
	}
}
func TestActualSingBoxSchema(t *testing.T) {
	engine := os.Getenv("YGG_TEST_SING_BOX")
	if engine == "" {
		t.Skip("set YGG_TEST_SING_BOX for engine schema validation")
	}
	path := filepath.Join(t.TempDir(), "config.json")
	raw, _ := json.Marshal(tunConfig(validProfile()))
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command(engine, "check", "-c", path).CombinedOutput(); err != nil {
		t.Fatalf("schema rejected: %v %s", err, out)
	}
}
