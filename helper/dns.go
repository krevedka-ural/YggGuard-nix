package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// Retain the original file OR symlink, not just its contents. A persistent backup
// also permits manual recovery after SIGKILL/power loss while the VPN was active.
func replaceResolver(path string) (func(), error) {
	backup := filepath.Join(filepath.Dir(path), ".yggguard-resolv.conf.original")
	if _, err := os.Lstat(backup); err == nil {
		return nil, errors.New("обнаружена резервная копия resolv.conf от прерванного сеанса; восстановите DNS по README")
	}
	if _, err := os.Lstat(path); err != nil {
		return nil, err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".yggguard-dns-")
	if err != nil {
		return nil, err
	}
	temp := f.Name()
	defer os.Remove(temp)
	if err = f.Chmod(0644); err != nil {
		f.Close()
		return nil, err
	}
	_, err = f.WriteString("# YggGuard active session\nnameserver 172.30.255.2\noptions timeout:2 attempts:2\n")
	closeErr := f.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	if err = os.Rename(path, backup); err != nil {
		return nil, err
	}
	if err = os.Rename(temp, path); err != nil {
		_ = os.Rename(backup, path)
		return nil, err
	}
	return func() { _ = os.Rename(backup, path) }, nil
}
func manageDNS() (func(), error) {
	if tool, err := exec.LookPath("resolvectl"); err == nil && exec.Command(tool, "status").Run() == nil {
		revert := func() { _ = exec.Command(tool, "revert", "yggguard-vpn").Run() }
		for _, args := range [][]string{{"dns", "yggguard-vpn", "172.30.255.2"}, {"domain", "yggguard-vpn", "~."}, {"default-route", "yggguard-vpn", "yes"}} {
			if out, err := exec.Command(tool, args...).CombinedOutput(); err != nil {
				revert()
				return nil, fmt.Errorf("настройка DNS: %s: %w", out, err)
			}
		}
		return revert, nil
	}
	return replaceResolver("/etc/resolv.conf")
}
