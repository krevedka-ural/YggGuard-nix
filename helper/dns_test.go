package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestResolverRegularFileRestored(t *testing.T) {
	path := filepath.Join(t.TempDir(), "resolv.conf")
	original := []byte("nameserver 192.0.2.1\n")
	if err := os.WriteFile(path, original, 0644); err != nil {
		t.Fatal(err)
	}
	restore, err := replaceResolver(path)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	if string(data) == string(original) {
		t.Fatal("DNS not configured")
	}
	if _, err = replaceResolver(path); err == nil {
		t.Fatal("must not overwrite recovery backup")
	}
	restore()
	data, _ = os.ReadFile(path)
	if string(data) != string(original) {
		t.Fatal("original DNS lost")
	}
}
func TestResolverSymlinkRestored(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "managed.conf")
	path := filepath.Join(dir, "resolv.conf")
	if err := os.WriteFile(target, []byte("nameserver 192.0.2.2\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, path); err != nil {
		t.Fatal(err)
	}
	restore, err := replaceResolver(path)
	if err != nil {
		t.Fatal(err)
	}
	original, _ := os.ReadFile(target)
	if string(original) != "nameserver 192.0.2.2\n" {
		t.Fatal("managed target changed")
	}
	restore()
	link, err := os.Readlink(path)
	if err != nil || link != target {
		t.Fatal("symlink not restored", link, err)
	}
}
