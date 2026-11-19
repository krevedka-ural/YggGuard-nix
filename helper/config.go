package main

import (
	"encoding/hex"
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
)

type Profile struct {
	Host       string   `json:"host"`
	Port       int      `json:"port"`
	Peers      []string `json:"peers"`
	PrivateKey string   `json:"private_key"`
}

func (p Profile) validate() error {
	ip := net.ParseIP(p.Host)
	_, network, _ := net.ParseCIDR("200::/7")
	if ip == nil || !network.Contains(ip) || strings.Contains(p.Host, "%") {
		return fmt.Errorf("нужен IPv6 Yggdrasil внутри 200::/7")
	}
	if p.Port < 1 || p.Port > 65535 {
		return fmt.Errorf("неверный порт прокси")
	}
	key, err := hex.DecodeString(p.PrivateKey)
	if err != nil || len(key) != 64 {
		return fmt.Errorf("неверный ключ узла Yggdrasil")
	}
	if len(p.Peers) == 0 || len(p.Peers) > 16 {
		return fmt.Errorf("укажите от 1 до 16 пиров Yggdrasil")
	}
	for _, peer := range p.Peers {
		u, err := url.Parse(peer)
		if err != nil || (u.Scheme != "tcp" && u.Scheme != "tls" && u.Scheme != "quic") || u.Hostname() == "" || u.User != nil || u.Fragment != "" || len(peer) > 2048 {
			return fmt.Errorf("неверный URI пира")
		}
		port, err := strconv.Atoi(u.Port())
		if err != nil || port < 1 || port > 65535 {
			return fmt.Errorf("у пира должен быть порт")
		}
	}
	return nil
}
func yggConfig(p Profile) map[string]any {
	return map[string]any{"PrivateKey": p.PrivateKey, "Peers": p.Peers, "Listen": []string{},
		"MulticastInterfaces": []any{}, "AdminListen": "none", "IfName": "yggguard0", "IfMTU": 65535,
		"NodeInfoPrivacy": true}
}
func tunConfig(p Profile) map[string]any {
	return map[string]any{
		"log": map[string]any{"level": "warn", "timestamp": true},
		"dns": map[string]any{"servers": []any{map[string]any{"type": "https", "tag": "secure-dns", "server": "1.1.1.1", "server_port": 443,
			"tls": map[string]any{"enabled": true, "server_name": "cloudflare-dns.com"}, "detour": "proxy"}}, "final": "secure-dns", "strategy": "ipv4_only"},
		"inbounds": []any{map[string]any{"type": "tun", "tag": "tun-in", "interface_name": "yggguard-vpn",
			"address": []string{"172.30.255.1/30", "fdfe:7967:6700::1/126"}, "mtu": 1400,
			"auto_route": true, "strict_route": true, "stack": "gvisor", "exclude_uid": []int{0},
			"iproute2_table_index": 20820, "iproute2_rule_index": 20820}},
		"outbounds": []any{map[string]any{"type": "http", "tag": "proxy", "server": p.Host, "server_port": p.Port}},
		"route": map[string]any{"auto_detect_interface": true, "final": "proxy", "rules": []any{
			map[string]any{"port": 53, "action": "hijack-dns"},
			map[string]any{"network": "udp", "action": "reject"}}},
	}
}
