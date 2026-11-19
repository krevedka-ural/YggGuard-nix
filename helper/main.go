// Linux privileged session supervisor. Only this process and its two engines run as root.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

type events struct {
	sync.Mutex
	w io.Writer
}

func (e *events) send(kind, text string) {
	e.Lock()
	defer e.Unlock()
	_ = json.NewEncoder(e.w).Encode(map[string]string{"type": kind, "text": text})
}

type child struct {
	cmd  *exec.Cmd
	done chan error
}

func start(ctx context.Context, e *events, name string, args ...string) (*child, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM}
	cmd.WaitDelay = 3 * time.Second
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = cmd.Stdout
	if err = cmd.Start(); err != nil {
		return nil, err
	}
	c := &child{cmd: cmd, done: make(chan error, 1)}
	go func() {
		scanner := bufio.NewScanner(out)
		for scanner.Scan() {
			e.send("log", scanner.Text())
		}
	}()
	go func() { c.done <- cmd.Wait() }()
	return c, nil
}
func (c *child) stop() {
	if c == nil {
		return
	}
	_ = c.cmd.Process.Signal(syscall.SIGTERM)
	select {
	case <-c.done:
	case <-time.After(5 * time.Second):
		_ = c.cmd.Process.Kill()
		<-c.done
	}
}
func writeConfig(dir, name string, v any) (string, error) {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return "", err
	}
	path := filepath.Join(dir, name)
	err = os.WriteFile(path, data, 0600)
	return path, err
}
func waitInterface(ctx context.Context) error {
	ticker := time.NewTicker(200 * time.Millisecond)
	defer ticker.Stop()
	for {
		iface, err := net.InterfaceByName("yggguard0")
		if err == nil {
			a, _ := iface.Addrs()
			if len(a) > 0 {
				return nil
			}
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}
func probe(ctx context.Context, p Profile) error {
	endpoint := &url.URL{Scheme: "http", Host: net.JoinHostPort(p.Host, fmt.Sprint(p.Port))}
	tr := &http.Transport{Proxy: http.ProxyURL(endpoint), TLSHandshakeTimeout: 15 * time.Second}
	defer tr.CloseIdleConnections()
	client := &http.Client{Transport: tr, Timeout: 18 * time.Second}
	var last error
	for {
		req, _ := http.NewRequestWithContext(ctx, "GET", "https://check.torproject.org/api/ip", nil)
		response, err := client.Do(req)
		if err == nil {
			var info struct {
				IP    string
				IsTor bool
			}
			err = json.NewDecoder(io.LimitReader(response.Body, 8192)).Decode(&info)
			response.Body.Close()
			if err == nil && response.StatusCode == 200 && net.ParseIP(info.IP) != nil {
				if p.Port == 2080 && !info.IsTor {
					return errors.New("порт 2080 отвечает без Tor; проверьте VPS")
				}
				return nil
			}
		}
		last = err
		select {
		case <-ctx.Done():
			return fmt.Errorf("нет ответа через прокси: %v (%w)", last, ctx.Err())
		case <-time.After(1 * time.Second):
		}
	}
}
func protectOverlay() (func(), error) {
	nft, err := exec.LookPath("nft")
	if err != nil {
		return nil, errors.New("установите nftables: защита входящих соединений Yggdrasil недоступна")
	}
	rules := "add table inet yggguard_client\nadd chain inet yggguard_client input { type filter hook input priority -10; policy accept; }\nadd rule inet yggguard_client input iifname \"yggguard0\" ct state established,related accept\nadd rule inet yggguard_client input iifname \"yggguard0\" drop\n"
	// Never modify a table that belongs to another instance or a previous interrupted session.
	if exec.Command(nft, "list", "table", "inet", "yggguard_client").Run() == nil {
		return nil, errors.New("осталась таблица yggguard_client; проверьте предыдущий сеанс перед повторным подключением")
	}
	cmd := exec.Command(nft, "-f", "-")
	cmd.Stdin = strings.NewReader(rules)
	if output, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("защита Yggdrasil: %s: %w", output, err)
	}
	return func() { _ = exec.Command(nft, "delete", "table", "inet", "yggguard_client").Run() }, nil
}

func session(ctx context.Context, e *events, p Profile, dir string) error {
	if err := p.validate(); err != nil {
		return err
	}
	lock, err := os.OpenFile("/run/yggguard-client.lock", os.O_CREATE|os.O_RDWR|syscall.O_NOFOLLOW, 0600)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err = syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		return errors.New("другое подключение YggGuard уже работает")
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
	if _, err = net.InterfaceByName("yggguard0"); err == nil {
		return errors.New("интерфейс yggguard0 уже существует")
	}
	ygg, err := writeConfig(dir, "ygg.json", yggConfig(p))
	if err != nil {
		return err
	}
	tun, err := writeConfig(dir, "tun.json", tunConfig(p))
	if err != nil {
		return err
	}
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	bin := filepath.Dir(executable)
	// Verify schema before creating interfaces or changing routes.
	check := exec.CommandContext(ctx, filepath.Join(bin, "sing-box"), "check", "-c", tun)
	if output, err := check.CombinedOutput(); err != nil {
		return fmt.Errorf("конфигурация sing-box: %s: %w", output, err)
	}
	cleanupFirewall, err := protectOverlay()
	if err != nil {
		return err
	}
	defer cleanupFirewall()
	e.send("status", "Подключение к Yggdrasil…")
	y, err := start(ctx, e, filepath.Join(bin, "yggdrasil"), "-useconffile", ygg)
	if err != nil {
		return err
	}
	defer y.stop()
	ready, cancel := context.WithTimeout(ctx, 100*time.Second)
	defer cancel()
	if err = waitInterface(ready); err != nil {
		return fmt.Errorf("Yggdrasil не создал интерфейс: %w", err)
	}
	e.send("status", "Проверка выхода в интернет…")
	if err = probe(ready, p); err != nil {
		return err
	}
	e.send("status", "Настройка VPN…")
	t, err := start(ctx, e, filepath.Join(bin, "sing-box"), "run", "-c", tun)
	if err != nil {
		return err
	}
	// Use explicit shutdown so sing-box removes routes before the overlay disappears.
	defer t.stop()
	select {
	case err = <-t.done:
		t.done <- err
		return fmt.Errorf("sing-box остановлен: %v", err)
	case <-time.After(1500 * time.Millisecond):
	case <-ctx.Done():
		return nil
	}
	cleanupDNS, err := manageDNS()
	if err != nil {
		return fmt.Errorf("не удалось направить системный DNS в VPN: %w", err)
	}
	defer cleanupDNS()
	e.send("connected", "Подключено")
	select {
	case <-ctx.Done():
		return nil
	case err = <-t.done:
		t.done <- err
		return fmt.Errorf("sing-box остановлен: %v", err)
	case err = <-y.done:
		y.done <- err
		return fmt.Errorf("Yggdrasil остановлен: %v", err)
	}
}
func run() error {
	if len(os.Args) != 3 {
		return errors.New("этот компонент запускается только приложением YggGuard")
	}
	if os.Geteuid() != 0 {
		return errors.New("для TUN нужны права администратора")
	}
	conn, err := net.DialTimeout("unix", os.Args[1], 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	e := &events{w: conn}
	e.send("hello", os.Args[2])
	_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	reader := bufio.NewReader(io.LimitReader(conn, 65537))
	line, err := reader.ReadBytes('\n')
	if err != nil {
		return err
	}
	if len(line) > 65536 {
		return errors.New("слишком большая конфигурация")
	}
	var p Profile
	if err = json.Unmarshal(line, &p); err != nil {
		return err
	}
	_ = conn.SetReadDeadline(time.Time{})
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP)
	defer cancel()
	go func() { _, _ = io.Copy(io.Discard, conn); cancel() }()
	dir, err := os.MkdirTemp("", "yggguard-session-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	// Children receive a manual, ordered SIGTERM; never kill both simultaneously on cancellation.
	if err = session(ctx, e, p, dir); err != nil {
		e.send("error", err.Error())
		return err
	}
	e.send("stopped", "Отключено")
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
