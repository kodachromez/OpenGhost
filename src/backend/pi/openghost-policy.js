// OpenGhost's Ask / Auto / Full access, for Pi's tools: which calls need the user's
// approval, and what the approval card shows. Ported from OpenGhost 1.3's
// agent-tools.js (needsApproval, describe and their command analysis; MIT, see
// LICENSE), mapped onto Pi's built-in tools: read and ls/grep/find look (1.3's
// read_file/list_files), write and edit change files, bash (and powershell) run
// commands. Any other tool (an extension's, an MCP server's) asks outside Full,
// as 1.3's unknown tools did.
//   Ask:  asks before running commands, changing files or looking outside the
//         chat's folder.
//   Auto: works in the chat's folder on its own; asks before risky commands,
//         files outside the folder, and other tools.
//   Full: never asks. Pi's tools keep the authority of the Pi process.
// Paths are resolved as Pi's tools resolve them (an `@` prefix, `~`, file URLs,
// and read's fallback names), and symlinks are followed, so a link inside the folder
// to a file outside it is outside. Paths are compared component by component with
// the host's own separators: on POSIX a backslash is part of a name. Command
// analysis is 1.3's: a heuristic over the command's text, never a sandbox.
import { existsSync, realpathSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, isAbsolute, join, relative as nodeRelative, resolve as nodeResolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const WINDOWS = process.platform === "win32";
const INSTALLS_MAC = /\bbrew\b[^\n;|]*\s(install|uninstall|upgrade|remove)\b/;
const INSTALLS_LINUX = /\b(apt|apt-get|dnf|yum|pacman|zypper)\b[^\n;|]*\s(install|remove|purge|upgrade|dist-upgrade)\b/;
// 1.3's English wording (i18n.js).
const TEXT = {
  command: "Run a command",
  write: "Write a file",
  edit: "Edit a file",
  read: "Read a file outside the project",
  list: "Look into a folder outside the project",
  home: "Home folder",
};
const UNICODE_SPACES = /[\u00A0\u2000-\u200A\u202F\u205F\u3000]/g; // As Pi's own paths

const READ_GIT = new Set(['status', 'log', 'diff', 'show', 'rev-parse', 'ls-files', 'blame', 'shortlog', 'describe', 'grep', 'help', 'version']);
const LIST_GIT = { branch: /^(-a|-r|-v|-vv|--list|--all|--show-current|--no-color)$/, remote: /^(-v|--verbose)$/, tag: /^(-l|--list)$/ };
const RISKY_POWERSHELL = [
 /\b(Remove-Item|rm|rmdir|rd|del|erase|ri|Clear-Content|Clear-Item)\b/i,
 /\b(Format-Volume|Format-Disk|Clear-Disk|Initialize-Disk|diskpart|bcdedit|cipher)\b/i,
 /\b(Stop-Computer|Restart-Computer|shutdown|logoff)\b/i,
 /\b(Stop-Process|spps|kill|taskkill|Stop-Service|Set-Service|sc\.exe)\b/i,
 /\b(Set-ExecutionPolicy|New-ItemProperty|Set-ItemProperty|Remove-ItemProperty|reg(\.exe)?\s+(add|delete|import|load))\b/i,
 /\b(HKLM|HKCU|HKCR|Registry)::?/i,
 /\b(winget|choco|scoop)\s+(install|uninstall|upgrade|remove)\b/i,
 /\b(npm|pnpm|yarn)\s+(i|install|add|remove|uninstall)\b[^\n;|]*\s(-g|--global)\b/i,
 /\bpip3?\s+(install|uninstall)\b[^\n;|]*--user\b/i,
 /-Verb\s+RunAs\b/i,
 /\b(Invoke-Expression|iex)\b/i,
 /\b(Set-Acl|icacls|takeown|attrib)\b/i,
 /\b(netsh|New-NetFirewallRule|Set-NetFirewallProfile|Disable-|Enable-WindowsOptionalFeature)\b/i,
 /\bgit\s+(push|pull|clean|rebase|reset\s+--hard|checkout\s+(--|-f|\.)|restore|filter-branch)\b/i,
 /\b(Send-MailMessage|New-PSSession|Enter-PSSession|Invoke-Command)\b/i,
];
const RISKY_POSIX = [
 /\brm\b/, /\bshred\b/, /\bmkfs\b/, /\bdd\b/, /\bshutdown\b/, /\breboot\b/, /\bhalt\b/, /\bpoweroff\b/,
 /\bsudo\b/, /\bsu\b/, /\bkill\b/, /\bkillall\b/, /\bpkill\b/, /\bsystemctl\b/, /\bservice\b/,
 INSTALLS_MAC,
 INSTALLS_LINUX,
 /\b(npm|pnpm|yarn)\s+(i|install|add|remove|uninstall)\b[^\n;|]*\s(-g|--global)\b/,
 /\bpip3?\s+(install|uninstall)\b[^\n;|]*--user\b/,
 /\b(chmod|chown)\b/, /\b(ufw|iptables|nft)\b/,
 /\bgit\s+(push|pull|clean|rebase|reset\s+--hard|checkout\s+(--|-f|\.)|restore|filter-branch)\b/,
 /\b(curl|wget)\b[^\n|]*\|\s*(ba)?sh\b/,
];
const OUTSIDE_POWERSHELL = [/(^|[^\w.])\.\.[\\/]/, /\$env:(USERPROFILE|HOMEPATH|APPDATA|LOCALAPPDATA|ProgramData|ProgramFiles|windir|SystemRoot|SystemDrive|OneDrive|PUBLIC)/i, /\$HOME\b/i, /(^|[\s'"(=,])~[\\/]/, /(^|[\s'"(=,])\\\\[\w.$-]+\\/];
const OUTSIDE_POSIX = [/(^|[^\w.])\.\.\//, /\$HOME\b/, /(^|[\s'"(=,])~\//, /(^|[\s'"(=,])\/(etc|usr|bin|sbin|boot|root|proc|sys|dev)\b/];
const ABSOLUTE_POWERSHELL = /(?:^|[\s'"(=,;|@])([a-zA-Z]:[\\/][^\s'"|;,)<>`]*)/g;
const ABSOLUTE_POSIX = /(?:^|[\s'"(=,;|@])(\/(?:[^\s'"|;,)<>`]|\\ )*)/g;

// Windows paths ignore case and take either slash; elsewhere they keep their case and
// use / alone (a backslash is an ordinary character of a name).
const PATHS = WINDOWS ? {
 sep: '\\', split: /[\\/]+/, home: /^~([\\/]|$)/, here: /^\.[\\/]/, trailing: /[\\/]+$/,
 absolute: path => /^[a-zA-Z]:[\\/]/.test(path) || path.startsWith('\\\\'),
 part: part => part.toLowerCase(),
 key: path => path.toLowerCase(),
} : {
 sep: '/', split: /\/+/, home: /^~(\/|$)/, here: /^\.\//, trailing: /(?<=.)\/+$/,
 absolute: path => path.startsWith('/'),
 part: part => part,
 key: path => path,
};

// The path a Pi tool uses for `path` (Pi's resolveToCwd: Unicode spaces, an `@`
// prefix, Git Bash drive paths on Windows, `~`, file URLs), with symlinks followed
// as far as the path exists.
function resolve(cwd, path) {
 const full = expand(cwd, path);
 return full && real(full);
}

// Pi's resolveToCwd itself, before symlinks are followed.
function expand(cwd, path) {
 let raw = String(path ?? '').replace(UNICODE_SPACES, ' ');
 if (raw.startsWith('@')) raw = raw.slice(1);
 if (WINDOWS && raw.startsWith('/') && !raw.startsWith('//') && !raw.includes('\\')) {
  const drive = /^\/(?:mnt\/|cygdrive\/)?([a-z])(?:\/(.*))?$/i.exec(raw);
  if (drive) raw = `${drive[1].toUpperCase()}:\\${drive[2]?.replaceAll('/', '\\') ?? ''}`;
 }
 if (raw === '~') raw = homedir();
 else if (raw.startsWith('~/') || (WINDOWS && raw.startsWith('~\\'))) raw = join(homedir(), raw.slice(2));
 if (/^file:\/\//.test(raw)) {
  try { raw = fileURLToPath(raw); } catch { return null; }
 }
 return nodeResolve(cwd || '.', raw || '.');
}

function real(path) {
 const rest = [];
 for (let head = path; ;) {
  try { return join(realpathSync.native(head), ...rest); } catch {}
  const up = dirname(head);
  if (up === head) return path;
  rest.unshift(basename(head));
  head = up;
 }
}

// Pi's read opens the first of these that exists (its resolveReadPath): the resolved
// name, then its macOS screenshot-spacing, NFD, curly-quote and NFD+curly-quote
// variants. The name and every variant Pi would find are decided on, so whichever
// one Pi opens was authorized; a variant Pi cannot find is never opened.
function readVariants(full) {
 const nfd = full.normalize('NFD');
 const variants = [full.replace(/ (AM|PM)\./gi, '\u202F$1.'), nfd, full.replace(/'/g, '\u2019'), nfd.replace(/'/g, '\u2019')];
 return [full, ...new Set(variants.filter(variant => variant !== full && existsSync(variant)))];
}

// Whether a resolved path is the root or below it: compared by path components with
// the host's own rules (node's path), never by text.
function within(root, target) {
 const rel = nodeRelative(root, target);
 return rel === '' || (rel !== '..' && !rel.startsWith(`..${sep}`) && !isAbsolute(rel));
}

function inside(cwd, path, read = false) {
 const full = cwd && expand(cwd, path);
 if (!full) return false;
 const root = real(nodeResolve(cwd));
 return (read ? readVariants(full) : [full]).every(candidate => within(root, real(candidate)));
}

function gitArgs(args) {
 if (typeof args === 'string') args = args.split(/\s+/).filter(Boolean);
 if (!Array.isArray(args)) return [];
 return args[0] === 'git' ? args.slice(1) : args;
}

function readOnlyGit(args) {
 const [command, ...rest] = gitArgs(args);
 if (READ_GIT.has(command)) return true;
 if (command === 'stash') return rest[0] === 'list' || rest[0] === 'show';
 const list = LIST_GIT[command];
 return !!list && rest.every(arg => list.test(arg));
}

function riskyShell(command, cwd, powershell) {
 const text = String(command || '');
 // A POSIX shell drops an unquoted backslash (`..\/` is `../`): both spellings are judged.
 const texts = powershell ? [text] : [...new Set([text, text.replace(/\\([\s\S])/g, '$1')])];
 return texts.some(text => {
  if ((powershell ? RISKY_POWERSHELL : RISKY_POSIX).some(re => re.test(text))) return true;
  if ((powershell ? OUTSIDE_POWERSHELL : OUTSIDE_POSIX).some(re => re.test(text))) return true;
  for (const match of text.matchAll(powershell ? ABSOLUTE_POWERSHELL : ABSOLUTE_POSIX)) if (!inside(cwd, match[1])) return true;
  return false;
 });
}

// What a step does, as the card shows it. The app tells it from the step itself, never from the agent's own words,
// so a harmless-sounding description can't hide a deletion; the strongest effect found wins. A command counts as
// only reading when every command in it only looks; a program, a method call or anything unknown counts as running something.
const SHELL_EFFECTS = [
 ['delete', new Set(['remove-item', 'clear-content', 'clear-item', 'clear-recyclebin', 'rm', 'rmdir', 'rd', 'del', 'erase', 'ri']), null],
 ['system', new Set(['format-volume', 'format-disk', 'clear-disk', 'initialize-disk', 'diskpart', 'bcdedit', 'stop-computer', 'restart-computer', 'shutdown', 'logoff', 'stop-process', 'spps', 'kill', 'taskkill', 'stop-service', 'start-service', 'restart-service', 'set-service', 'sc.exe', 'set-executionpolicy', 'new-itemproperty', 'set-itemproperty', 'remove-itemproperty', 'set-acl', 'icacls', 'takeown', 'attrib', 'netsh', 'setx', 'new-netfirewallrule', 'set-netfirewallprofile', 'enable-windowsoptionalfeature', 'disable-windowsoptionalfeature']),
  /\breg(\.exe)?\s+(add|delete|import|load)\b|\b(HKLM|HKCU|HKCR|Registry)::?|-Verb\s+RunAs\b/i],
 ['install', new Set(['install-module', 'install-package', 'install-script', 'uninstall-module', 'uninstall-package', 'update-module']),
  /\b(winget|choco|scoop)\s+(install|uninstall|upgrade|remove)\b|\b(npm|pnpm|yarn|bun)\s+(i|install|add|remove|uninstall|ci|update)\b|\bpip3?\s+(install|uninstall)\b/i],
 ['change', new Set(['set-content', 'add-content', 'out-file', 'new-item', 'set-item', 'copy-item', 'move-item', 'rename-item', 'expand-archive', 'compress-archive', 'export-csv', 'export-clixml', 'tee-object', 'mkdir', 'md', 'ni', 'cpi', 'copy', 'cp', 'mi', 'move', 'mv', 'ren', 'rni', 'sc', 'ac', 'tee']),
  /(^|[^-=<>2*])>>?\s*(?!&|\$null\b)\S/],
 ['online', new Set(['invoke-webrequest', 'iwr', 'invoke-restmethod', 'irm', 'curl', 'wget', 'start-bitstransfer', 'test-netconnection', 'test-connection', 'ping', 'resolve-dnsname', 'send-mailmessage']),
  /\bgit\s+(clone|fetch|pull|push)\b/i],
];
const READ_COMMAND = /^(get|select|sort|format|measure|where|group|test|resolve|split|join|compare|convertto|convertfrom|find)-\w+$|^(out-string|out-host|write-output|write-host|foreach-object|set-location|push-location|pop-location|start-sleep)$/;
const READ_WORDS = new Set(['ls', 'dir', 'gci', 'cat', 'type', 'gc', 'gi', 'gp', 'gm', 'sls', 'ft', 'fl', 'fw', 'select', 'sort', 'where', 'measure', 'group', 'compare', 'echo', 'write', 'pwd', 'cd', 'chdir', 'sl', 'foreach', 'if', 'else', 'elseif', 'switch', 'for', 'while', 'do', 'try', 'catch', 'finally', 'return', 'param', 'begin', 'process', 'end']);
const FILE_COMMANDS = new Set([...SHELL_EFFECTS[0][1], ...SHELL_EFFECTS[3][1], 'get-childitem', 'get-content', 'get-item', 'ls', 'dir', 'gci', 'cat', 'type', 'gc', 'gi', 'test-path', 'invoke-item', 'ii', 'start-process', 'start']);
const PATH_PARAMS = /^-(path|literalpath|destination|outfile|filepath|target|workingdirectory)$/i;
const HOME = WINDOWS ? /^(\$HOME|\$env:USERPROFILE|~)(?=[\\/]|$)/i : /^(\$HOME|\$\{HOME\}|~)(?=\/|$)/;

// Statements and pipeline stages, script blocks and subexpressions included, split outside of quotes.
function segments(code) {
 const parts = [];
 let part = '', quote = '';
 for (let i = 0; i < code.length; i++) {
  const c = code[i];
  if (quote) {
   part += c;
   if (c === '`' && quote === '"') part += code[++i] ?? '';
   else if (c === quote) quote = '';
  } else if (c === '"' || c === "'") {
   quote = c;
   part += c;
  } else if ('|;\n{}()'.includes(c) || (c === '&' && code[i + 1] === '&')) {
   parts.push(part);
   part = '';
  } else part += c;
 }
 parts.push(part);
 return parts.map(item => item.trim().replace(/^(\$[\w:.]+\s*=\s*)+/, '').replace(/^[&.]\s*/, '')).filter(Boolean);
}

function tokens(segment) {
 return [...segment.matchAll(/"((?:[^"`]|`.)*)"|'([^']*)'|(\S+)/g)].map(m => m[1] ?? m[2] ?? m[3]);
}

function shellEffect(code) {
 const parts = segments(code), words = parts.map(part => part.split(/\s+/)[0].toLowerCase());
 const text = code.replace(/'[^']*'|"(?:[^"`]|`.)*"/g, '""');
 for (const [effect, commands, pattern] of SHELL_EFFECTS) if (words.some(word => commands.has(word)) || pattern?.test(text)) return effect;
 const reads = words.every(word => /^[$\d-]/.test(word) ? !/\.\w+$/.test(word) : READ_COMMAND.test(word) || READ_WORDS.has(word));
 return parts.length && reads && !/::/.test(text) ? 'read' : 'run';
}

// The places a command reaches: paths handed to file commands, files it writes with > and anything under
// the home folder or given in full. Variables and registry keys aren't places a person would recognise.
function shellTargets(code, cwd) {
 const found = [];
 for (const part of segments(code)) {
  const [command, ...rest] = tokens(part), file = FILE_COMMANDS.has(String(command).toLowerCase());
  let positional = file;
  for (let i = 0; i < rest.length; i++) {
   const arg = rest[i];
   if (PATH_PARAMS.test(arg)) { found.push(rest[++i]); continue; }
   const redirect = /^\d?>>?(.*)$/.exec(arg);
   if (redirect) { found.push(redirect[1] || rest[++i]); continue; }
   if (arg.startsWith('-')) { positional = positional && !/^-(filter|include|exclude|pattern|encoding|value|itemtype|name|newname|argumentlist)$/i.test(arg); continue; }
   if (positional || HOME.test(arg) || /^[a-zA-Z]:[\\/]/.test(arg)) found.push(arg);
   positional = false;
  }
 }
 const places = found.filter(path => path && !/^\$(?!HOME\b|env:USERPROFILE\b)|^(HK\w+|Registry)::?/i.test(path)).map(path => place(path, cwd));
 return distinct(places);
}

// bash and zsh on a Mac and on Linux. A line's command is found past sudo, env, nohup, xargs and the like; find -exec and
// sh -c count for the command they run, git counts the way the git tool does, and a heredoc's text is data, not commands.
const POSIX_EFFECTS = [
 ['delete', ['rm', 'rmdir', 'unlink', 'shred', 'srm', 'trash'], [/\bfind\b[^\n;|&]*\s-delete\b/, /\brsync\b[^\n;|&]*\s--delete/]],
 ['system', ['sudo', 'doas', 'su', 'shutdown', 'reboot', 'halt', 'poweroff', 'kill', 'killall', 'pkill', 'dd', 'fdisk', 'sfdisk', 'parted', 'mkswap', 'swapon', 'swapoff', 'umount', 'passwd', 'useradd', 'userdel', 'usermod', 'groupadd', 'groupdel', 'visudo', 'update-alternatives', 'ldconfig', 'modprobe', 'insmod', 'rmmod', 'csrutil', 'spctl', 'tmutil', 'hostnamectl', 'timedatectl'], [
  /\bmkfs(\.\w+)?\b/,
  /\b(systemctl|launchctl)\s+(-{1,2}\S+\s+)*(start|stop|restart|reload|enable|disable|mask|unmask|kill|daemon-reload|set-default|isolate|load|unload|bootstrap|bootout|kickstart|remove|submit)\b/,
  /\bservice\s+\S+\s+(start|stop|restart|reload|force-reload)\b/,
  /\bdefaults\s+(-\S+\s+)*(write|delete|import|rename)\b/,
  /\bcrontab\s+(?!-l\b)[^\s|;&]/,
  /\bsysctl\s+(-w|--write)\b/,
  /\bdiskutil\s+(?!list\b|info\b|activity\b)\w/,
  /\bnetworksetup\s+-set/,
  /\bpmset\s+(?!-g\b)[^\s|;&]/,
  /\bscutil\s+--set\b/,
  /\bnvram\s+(-d|-c|\S+=)/,
  /\bufw\s+(?!status\b)\w/,
  /\b(iptables|ip6tables)\s+(\S+\s+)*-[ADIFXNPRZ]\b/,
  /\bnft\s+(add|delete|flush|insert|replace|create|destroy)\b/,
  /\bfirewall-cmd\s+(?!--(list|state|get|query))\S/,
  /\bmount\s+[^\s|;&]/,
  /\bip\s+(-\S+\s+)*(link|addr|address|route|rule|neigh)\s+(add|del|delete|set|flush|change|replace)\b/,
  /\bifconfig\s+\S+\s+(up|down|inet6?|alias|-alias|mtu|ether|lladdr)\b/,
 ]],
 ['install', [], [
  /\bbrew\s+(-\S+\s+)*(install|uninstall|reinstall|upgrade|remove|rm|tap|untap|link|unlink)\b/,
  /\b(apt|apt-get|aptitude)\s+(-\S+\s+)*(install|reinstall|remove|purge|upgrade|full-upgrade|dist-upgrade|autoremove)\b/,
  /\b(dnf|yum|microdnf)\s+(-\S+\s+)*(install|reinstall|remove|erase|upgrade|update|downgrade|autoremove)\b/,
  /\bzypper\s+(-\S+\s+)*(install|in|remove|rm|update|up|dist-upgrade|dup)\b/,
  /\b(pacman|yay|paru)\s+-[A-Za-z]*[SRU]/,
  /\bapk\s+(add|del|upgrade)\b/,
  /\b(snap|flatpak)\s+(install|remove|uninstall|refresh|update)\b/,
  /\bport\s+(install|uninstall|upgrade)\b/,
  /\bsoftwareupdate\s+(\S+\s+)*(-i|--install|-a|--all)\b/,
  /\b(npm|pnpm|yarn|bun)\s+(i|install|add|remove|uninstall|ci|update)\b/,
  /\b(pip3?|pipx)\s+(install|uninstall)\b|\bpython3?\s+-m\s+pip\s+(install|uninstall)\b/,
  /\buv\s+(pip\s+(install|uninstall)|add|remove|sync|tool\s+install)\b/,
  /\b(gem|cargo)\s+(install|uninstall)\b|\bgo\s+install\b/,
  /\b(conda|mamba|micromamba)\s+(install|remove|update|create)\b|\bpoetry\s+(add|remove|install)\b/,
  /\b(curl|wget)\b[^\n;|]*\|\s*(sudo\s+)?(ba|z|da)?sh\b/,
 ]],
 ['change', ['cp', 'mv', 'mkdir', 'touch', 'ln', 'tee', 'install', 'rsync', 'truncate', 'unzip', 'zip', 'gzip', 'gunzip', 'bzip2', 'bunzip2', 'xz', 'unxz', 'zstd', 'unzstd', '7z', 'patch', 'split', 'csplit', 'ditto', 'rename', 'mktemp', 'chmod', 'chown', 'chgrp', 'chflags', 'chattr', 'setfacl', 'xattr'], [
  /(^|[^<>&\d])(\d*|&)>>?\|?\s*(?!&|\/dev\/(null|stdout|stderr|tty)\b|\()[^\s;|&<>]/,
  /\bsed\b[^\n;|&]*\s(-[a-zA-Z]*i|--in-place)/,
  /\bperl\s+-[a-zA-Z]*i/,
  /\btar\s+(-{0,2}[a-zA-Z]*[cxru]|[^\n;|&]*\s-[a-zA-Z]*[cxru]|[^\n;|&]*\s--(create|extract|get|append|update)\b)/,
 ]],
 ['online', ['curl', 'wget', 'ssh', 'scp', 'sftp', 'ftp', 'lftp', 'mosh', 'nc', 'netcat', 'ncat', 'telnet', 'ping', 'ping6', 'dig', 'nslookup', 'host', 'whois', 'traceroute', 'tracepath', 'mtr', 'http', 'https', 'aria2c', 'gh'], [
  /\b(npm|pnpm|yarn)\s+publish\b|\bdocker\s+(pull|push|login)\b/,
 ]],
].map(([effect, commands, patterns]) => [effect, new Set(commands), patterns]);
const POSIX_RANK = ['delete', 'system', 'install', 'change', 'online', 'record', 'run', 'read'];
const POSIX_READS = new Set(['ls', 'll', 'la', 'l', 'dir', 'vdir', 'cat', 'bat', 'batcat', 'head', 'tail', 'less', 'more', 'grep', 'egrep', 'fgrep', 'zgrep', 'rg', 'ag', 'ack', 'find', 'fd', 'fdfind', 'locate', 'mdfind', 'mdls', 'wc', 'echo', 'printf', 'print', 'pwd', 'cd', 'pushd', 'popd', 'dirs', 'stat', 'file', 'du', 'df', 'which', 'whereis', 'type', 'hash', 'command', 'whoami', 'id', 'groups', 'users', 'who', 'w', 'last', 'uname', 'hostname', 'arch', 'sw_vers', 'lsb_release', 'date', 'cal', 'uptime', 'env', 'printenv', 'export', 'set', 'unset', 'local', 'declare', 'typeset', 'readonly', 'alias', 'unalias', 'shopt', 'setopt', 'read', 'sort', 'uniq', 'cut', 'tr', 'awk', 'gawk', 'mawk', 'sed', 'column', 'nl', 'fold', 'fmt', 'paste', 'join', 'comm', 'diff', 'cmp', 'colordiff', 'tree', 'realpath', 'readlink', 'dirname', 'basename', 'test', '[', '[[', ']', ']]', 'true', 'false', ':', 'sleep', 'wait', 'ps', 'pgrep', 'pidof', 'lsof', 'top', 'htop', 'free', 'vm_stat', 'iostat', 'vmstat', 'jq', 'yq', 'xxd', 'hexdump', 'od', 'strings', 'md5', 'md5sum', 'shasum', 'sha1sum', 'sha256sum', 'sha512sum', 'cksum', 'b2sum', 'seq', 'history', 'man', 'help', 'info', 'apropos', 'whatis', 'tty', 'locale', 'getconf', 'nproc', 'lscpu', 'lsblk', 'lsusb', 'lspci', 'ioreg', 'system_profiler', 'sysctl', 'defaults', 'systemctl', 'launchctl', 'service', 'crontab', 'diskutil', 'networksetup', 'pmset', 'scutil', 'nvram', 'ufw', 'iptables', 'ip6tables', 'nft', 'firewall-cmd', 'mount', 'ip', 'ifconfig', 'netstat', 'ss', 'tar', 'clear', 'tput', 'if', 'then', 'else', 'elif', 'fi', 'for', 'while', 'until', 'do', 'done', 'case', 'esac', 'in', 'function', 'return', 'exit', 'break', 'continue', 'select']);
// Commands whose arguments are places, and what their options take.
const POSIX_FILES = new Set(['rm', 'rmdir', 'unlink', 'shred', 'srm', 'trash', 'cp', 'mv', 'mkdir', 'touch', 'ln', 'tee', 'install', 'rsync', 'scp', 'truncate', 'tar', 'unzip', 'zip', 'gzip', 'gunzip', 'bzip2', 'bunzip2', 'xz', 'unxz', 'zstd', 'unzstd', '7z', 'split', 'ditto', 'chmod', 'chown', 'chgrp', 'chflags', 'chattr', 'xattr', 'cat', 'bat', 'batcat', 'head', 'tail', 'less', 'more', 'ls', 'll', 'la', 'stat', 'file', 'du', 'tree', 'wc', 'diff', 'cmp', 'md5', 'md5sum', 'shasum', 'sha256sum', 'realpath', 'readlink', 'open', 'xdg-open', 'grep', 'egrep', 'fgrep', 'zgrep', 'rg', 'ag', 'sed', 'awk', 'find', 'source', '.', 'python', 'python3', 'node', 'bash', 'sh', 'zsh', 'ruby', 'perl', 'php', 'gcc', 'g++', 'clang', 'clang++', 'cc', 'rustc']);
const POSIX_FIRST = new Set(['python', 'python3', 'node', 'bash', 'sh', 'zsh', 'ruby', 'perl', 'php', 'source', '.']);
const POSIX_LEADING = { chmod: 1, chown: 1, chgrp: 1, chflags: 1, grep: 1, egrep: 1, fgrep: 1, zgrep: 1, rg: 1, ag: 1, sed: 1, awk: 1 };
const GREP_VALUES = ['-e', '-f', '-m', '-A', '-B', '-C', '--regexp', '--file', '--max-count'];
const POSIX_VALUES = {
 head: ['-n', '-c'], tail: ['-n', '-c'], grep: GREP_VALUES, egrep: GREP_VALUES, fgrep: GREP_VALUES, zgrep: GREP_VALUES,
 rg: [...GREP_VALUES, '-g', '-t', '-T', '-j', '--glob', '--type'], ag: ['-A', '-B', '-C', '-G', '-m'],
 sed: ['-e', '-f', '--expression', '--file'], awk: ['-F', '-v', '-f'],
 cut: ['-d', '-f', '-c', '-b'], sort: ['-k', '-t', '-S'], du: ['-d', '-t', '-B'], ls: ['-I', '-w'], tree: ['-L', '-I', '-P'],
 mkdir: ['-m'], install: ['-m', '-o', '-g'], touch: ['-t', '-d'], split: ['-l', '-b', '-n', '-a'], unzip: ['-x'], zip: ['-x', '-i'],
 stat: ['-f', '-c', '--format'], diff: ['-U', '-C', '--label'], rsync: ['-e', '--exclude', '--include', '-f', '--filter'],
 tar: ['-b', '--exclude'], open: ['-a', '-b'], scp: ['-P', '-o', '-c', '-l'],
};
const POSIX_PATH_OPTIONS = {
 tar: ['-f', '-C', '-T', '-X', '--file', '--directory', '--files-from', '--exclude-from'], git: ['-C'], make: ['-C', '-f', '--directory', '--file'],
 unzip: ['-d'], wget: ['-O', '-P', '--output-document', '--directory-prefix'], curl: ['-o', '--output', '-T', '--upload-file'],
 cp: ['-t', '--target-directory'], mv: ['-t', '--target-directory'], ln: ['-t', '--target-directory'], install: ['-t', '--target-directory'],
 sort: ['-o', '--output'], touch: ['-r', '--reference'], gcc: ['-o'], 'g++': ['-o'], clang: ['-o'], 'clang++': ['-o'], cc: ['-o'], go: ['-o'], rustc: ['-o'],
 ssh: ['-i', '-F'], scp: ['-i', '-F'], sftp: ['-i', '-F'], pip: ['-r', '--requirement', '-t', '--target'], pip3: ['-r', '--requirement', '-t', '--target'],
 patch: ['-i', '-o', '-d', '--input', '--output', '--directory'], openssl: ['-in', '-out', '-keyout'], ffmpeg: ['-i'],
};
const POSIX_STOPS = { python: ['-c', '-m'], python3: ['-c', '-m'], node: ['-e', '-p', '--eval', '--print'], bash: ['-c'], sh: ['-c'], zsh: ['-c'], ruby: ['-e'], perl: ['-e'], php: ['-r'] };
// Words that lead to the command they run, with their options that take a value.
const POSIX_PREFIXES = {
 sudo: ['-u', '-g', '-C', '-D', '-h', '-p', '-r', '-t', '-U'], doas: ['-u', '-C'], nohup: [], time: [], nice: ['-n'], ionice: ['-c', '-n', '-p'], exec: ['-a'],
 builtin: [], noglob: [], caffeinate: ['-t', '-w'], env: ['-u', '-C', '-S'], command: [], xargs: ['-n', '-I', '-P', '-L', '-s', '-d', '-E', '-a'],
 timeout: ['-s', '-k', '--signal', '--kill-after'], watch: ['-n', '-d'], stdbuf: ['-i', '-o', '-e'], unbuffer: [],
 if: [], then: [], else: [], elif: [], while: [], until: [], do: [], '!': [],
};
const REDIRECT = /^(\d*|&)(>>?|<<?<?|<>)[&|-]?$/;
const SHELL_NAMES = /^(ba|z|da|k)?sh$/;

// Commands and pipeline stages, split outside of quotes. A command substitution is a command of its own while the one
// around it goes on; heredoc text and comments are left out, also from the whole line kept as .text.
function posixSegments(code) {
 const parts = [], outer = [], heredocs = [], dropped = [];
 let part = '', quote = '';
 const cut = () => { parts.push(part); part = ''; };
 const open = close => { outer.push({ part, quote, close }); part = ''; quote = ''; };
 const shut = () => { const top = outer.pop(); cut(); part = `${top.part}""`; quote = top.quote; };
 const past = (from, close) => { const end = code.indexOf(close, from); return end < 0 ? code.length : end + close.length; };
 for (let i = 0; i < code.length; i++) {
  const c = code[i], next = code[i + 1] ?? '';
  if (quote === "'") { part += c; if (c === "'") quote = ''; continue; }
  if (c === '\\') { part += c + next; i++; continue; }
  if (c === '$' && next === '(' && code[i + 2] === '(') { const end = past(i + 3, '))'); part += code.slice(i, end); i = end - 1; continue; }
  if (c === '$' && next === '(') { i++; open(')'); continue; }
  if (c === '`') { if (outer.at(-1)?.close === '`') shut(); else open('`'); continue; }
  if (quote === '"') { part += c; if (c === '"') quote = ''; continue; }
  if (c === ')' && outer.at(-1)?.close === ')') { shut(); continue; }
  if (c === '"' || c === "'") { quote = c; part += c; continue; }
  if (c === '$' && next === '{') { const end = past(i + 2, '}'); part += code.slice(i, end); i = end - 1; continue; }
  if (c === '#' && (!part || /\s$/.test(part))) {
   const end = code.indexOf('\n', i), stop = end < 0 ? code.length : end;
   dropped.push([i, stop]);
   i = stop - 1;
   continue;
  }
  if (c === '<' && next === '<' && code[i + 2] !== '<') {
   const m = /^<<(-?)[ \t]*(?:'([^'\n]*)'|"([^"\n]*)"|\\?([^\s;&|<>()]+))/.exec(code.slice(i, i + 200));
   if (m) { heredocs.push([m[2] ?? m[3] ?? m[4], !!m[1]]); part += m[0]; i += m[0].length - 1; continue; }
  }
  if (c === '\n') {
   cut();
   const from = i + 1;
   for (const [word, tabs] of heredocs.splice(0)) {
    for (;;) {
     const end = code.indexOf('\n', i + 1), line = code.slice(i + 1, end < 0 ? code.length : end);
     i = end < 0 ? code.length : end;
     if (end < 0 || (tabs ? line.replace(/^\t+/, '') : line).trimEnd() === word) break;
    }
   }
   if (i >= from) dropped.push([from, i]);
   continue;
  }
  if (c === ';' || c === '(' || c === ')') { cut(); continue; }
  if (c === '|') {
   if (part.endsWith('>')) part += c;
   else { cut(); if (next === '|' || next === '&') i++; }
   continue;
  }
  if (c === '&') {
   if (next === '&') { cut(); i++; }
   else if (next === '>' || /[<>]$/.test(part)) part += c;
   else cut();
   continue;
  }
  if ((c === '{' || c === '}') && (!part || /\s$/.test(part)) && (!next || /[\s;]/.test(next))) { cut(); continue; }
  part += c;
 }
 while (outer.length) shut();
 cut();
 const list = parts.map(item => item.trim()).filter(Boolean);
 list.text = dropped.reduceRight((text, [from, to]) => text.slice(0, from) + text.slice(to), code);
 return list;
}

// The words of a command with quotes and escapes resolved; redirections are words of their own.
function posixTokens(part) {
 const list = [];
 let token = '', has = false, quote = '';
 const flush = () => { if (has) list.push(token); token = ''; has = false; };
 for (let i = 0; i < part.length; i++) {
  const c = part[i];
  if (quote === "'") { if (c === "'") quote = ''; else token += c; continue; }
  if (quote === '"') {
   if (c === '"') quote = '';
   else if (c === '\\' && '"\\$`\n'.includes(part[i + 1] || 'x')) { i++; if (part[i] !== '\n') token += part[i]; }
   else token += c;
   continue;
  }
  if (c === '\\') { i++; if (i < part.length && part[i] !== '\n') { token += part[i]; has = true; } continue; }
  if (c === "'" || c === '"') { quote = c; has = true; continue; }
  if (/\s/.test(c)) { flush(); continue; }
  if (c === '<' || c === '>') {
   let op = has && /^(\d+|&)$/.test(token) ? token : '';
   if (op) { token = ''; has = false; } else flush();
   op += c;
   while ('<>'.includes(part[i + 1] || 'x') && op.length < 5) op += part[++i];
   if ('&|-'.includes(part[i + 1] || 'x')) op += part[++i];
   list.push(op);
   continue;
  }
  token += c;
  has = true;
 }
 flush();
 return list;
}

// The command a line runs, past assignments, redirections and words like sudo, env or xargs.
function posixCommand(tokens) {
 let k = 0, name = '';
 for (;;) {
  while (k < tokens.length && (REDIRECT.test(tokens[k]) || /^[A-Za-z_]\w*=/.test(tokens[k]))) k += REDIRECT.test(tokens[k]) ? 2 : 1;
  if (k >= tokens.length) return { name: name || ':', at: k };
  name = tokens[k].replace(/^.*\//, '');
  const values = POSIX_PREFIXES[name];
  if (!values || (name === 'command' && /^-[vV]$/.test(tokens[k + 1] || ''))) return { name: name.startsWith('-') ? ':' : name, at: k };
  k++;
  while (k < tokens.length && /^-./.test(tokens[k])) k += values.includes(tokens[k]) ? 2 : 1;
  if (name === 'timeout' && k < tokens.length) k++;
 }
}

// Git's own options come before its command; -C and -c take a value.
function gitCommand(args) {
 let k = 0;
 while (k < args.length && args[k].startsWith('-')) k += /^-[Cc]$/.test(args[k]) ? 2 : 1;
 return args.slice(k);
}

function posixEffect(code) {
 const parts = posixSegments(code), words = [], more = [];
 for (const part of parts) {
  const tokens = posixTokens(part), { name, at } = posixCommand(tokens), args = tokens.slice(at + 1);
  if (name === 'git') { more.push(gitEffect(gitCommand(args))); continue; }
  const inner = SHELL_NAMES.test(name) ? args.indexOf('-c') : -1;
  if (inner >= 0) { more.push(posixEffect(args[inner + 1] || '')); continue; }
  words.push(name);
  if (name === 'find') args.forEach((arg, k) => { if (/^-(exec|execdir|ok|okdir)$/.test(arg)) words.push(posixCommand(args.slice(k + 1)).name); });
 }
 const text = parts.text.replace(/'[^']*'|"(?:[^"\\]|\\.)*"/g, '""');
 const found = POSIX_EFFECTS.find(([, commands, patterns]) => words.some(word => commands.has(word)) || patterns.some(re => re.test(text)));
 const own = found ? found[0] : parts.length && words.every(word => POSIX_READS.has(word)) ? 'read' : 'run';
 return more.reduce((best, effect) => POSIX_RANK.indexOf(effect) < POSIX_RANK.indexOf(best) ? effect : best, own);
}

// Paths a command reaches, as written: arguments of file commands, values of options that take a path, files written
// with > and anything under the home folder or given in full.
function posixPaths(code) {
 const found = [];
 for (const part of posixSegments(code)) {
  const tokens = posixTokens(part), { name, at } = posixCommand(tokens), command = tokens[at] || '';
  if (/^\.{1,2}\//.test(command) || HOME.test(command)) found.push(command);
  const paths = POSIX_PATH_OPTIONS[name] || [], values = POSIX_VALUES[name] || [], stops = POSIX_STOPS[name] || [];
  let collect = POSIX_FILES.has(name), leading = POSIX_LEADING[name] || 0, options = true;
  for (let k = at + 1; k < tokens.length; k++) {
   const arg = tokens[k];
   if (REDIRECT.test(arg)) {
    if (/^[^<]*>[>|]?$/.test(arg)) found.push(tokens[k + 1]);
    k++;
    continue;
   }
   if (options && /^-./.test(arg)) {
    if (arg === '--') { options = false; continue; }
    const eq = arg.indexOf('='), flag = eq > 0 ? arg.slice(0, eq) : arg, value = () => eq > 0 ? arg.slice(eq + 1) : tokens[++k];
    if (stops.includes(flag)) {
     const inner = value();
     if (flag === '-c' && SHELL_NAMES.test(name)) found.push(...posixPaths(inner || ''));
     collect = false;
    } else if (paths.includes(flag)) found.push(value());
    else if (values.includes(flag)) {
     value();
     if (/^-(e|f)$|^--(regexp|file|expression)$/.test(flag)) leading = 0;
    } else if (name === 'sed' && arg === '-i' && /^(\.\w*)?$/.test(tokens[k + 1] ?? 'x')) k++;
    else if (name === 'xattr' && /^-[a-z]*[dpw]/.test(arg)) leading = /w/.test(arg) ? 2 : 1;
    else if (name === 'find') collect = false;
    continue;
   }
   if (collect && leading > 0) { leading--; continue; }
   if (collect) { found.push(arg); if (POSIX_FIRST.has(name)) collect = false; continue; }
   if (HOME.test(arg) || /^\/./.test(arg)) found.push(arg);
  }
 }
 return found;
}

// Variables, devices, web and remote addresses and numbers aren't places a person would recognise.
function posixTargets(code, cwd) {
 const places = posixPaths(code)
  .filter(path => path && path !== '{}' && path !== '-' && !/^\d+$|^\/dev\/|:\/\/|^[\w.@-]+:/.test(path) && !/^\$(?!HOME\b|\{HOME\})/.test(path))
  .map(path => place(path, cwd));
 return distinct(places);
}


// A path as a chip: named by its last part, with the whole path to hover over; the home folder and the project have
// names of their own, and a wildcard keeps the folder it looks in.
const partsOf = path => path.replace(HOME, '').split(/[\\/]/).filter(part => part && part !== '.');

function place(path, cwd) {
 const raw = String(path).replace(PATHS.trailing, ''), parts = partsOf(raw), last = parts.at(-1);
 const label = !last ? (HOME.test(raw) ? TEXT.home : raw === '/' ? '/' : String(cwd || '').split(/[\\/]/).filter(Boolean).pop() || raw)
  : /[*?]/.test(last) && parts.length > 1 ? parts.slice(-2).join(PATHS.sep) : last;
 return { kind: /\.[\w*]{1,8}$/.test(last || '') ? 'file' : 'folder', label, title: raw };
}

// The same place once; two places that share a name are told apart by the folder they are in.
function distinct(places) {
 const list = [...new Map(places.map(item => [PATHS.key(item.title), item])).values()];
 const shared = new Set(list.map(item => item.label).filter((label, i, all) => all.indexOf(label) !== i));
 for (const item of list) {
  const parts = partsOf(item.title);
  if (shared.has(item.label) && parts.length > 1) item.label = parts.slice(-2).join(PATHS.sep);
 }
 return list;
}

// Git either talks to a server, throws work away, only records history (a commit touches no file of the user's),
// or changes the files in the folder, like switching branches or merging.
function gitEffect(args) {
 const [command = '', ...rest] = gitArgs(args);
 if (/^(push|pull|fetch|clone)$/.test(command)) return 'online';
 if (/^(clean|restore|rm)$/.test(command) || (command === 'reset' && rest.includes('--hard'))
  || (command === 'checkout' && rest.some(arg => arg === '--' || arg === '.'))
  || (command === 'branch' && rest.some(arg => /^(-D|-d|--delete)$/.test(arg))) || (command === 'stash' && /^(drop|clear)$/.test(rest[0] || ''))) return 'delete';
 if (readOnlyGit(args)) return 'read';
 return /^(add|commit|init|tag|branch|notes)$/.test(command) ? 'record' : 'change';
}


const READS = new Set(['read', 'ls', 'grep', 'find']);

// Whether a call needs the user's approval in this mode (1.3's needsApproval).
export function needsApproval(name, input, { mode, cwd }) {
 if (mode === 'full') return false;
 const ask = mode !== 'auto'; // Anything else is Ask: never less than asked for.
 const args = input && typeof input === 'object' ? input : {};
 switch (name) {
  case 'read': return ask && !inside(cwd, args.path || '.', true);
  case 'ls':
  case 'grep':
  case 'find': return ask && !inside(cwd, args.path || '.');
  case 'write':
  case 'edit': return ask || !inside(cwd, args.path);
  case 'bash': return ask || riskyShell(args.command, cwd, false);
  case 'powershell': return ask || riskyShell(args.command, cwd, true);
  default: return true;
 }
}

function relative(cwd, path) {
 const raw = String(path ?? '.').trim() || '.';
 if (!PATHS.absolute(raw)) return raw.replace(PATHS.here, '');
 return inside(cwd, raw) ? nodeRelative(real(nodeResolve(cwd)), resolve(cwd, raw)) || '.' : raw;
}

// What the approval card shows (1.3's describe): a headline in plain words, what the
// step does (worked out from the step itself, never from the model's words), the
// places it touches, and the command or changes behind a reveal.
export function describe(name, input, cwd) {
 const args = input && typeof input === 'object' ? input : {};
 const file = { kind: 'file', label: String(args.path || '').split(/[\\/]/).filter(Boolean).pop() || String(args.path || ''), title: relative(cwd, args.path || '.') };
 switch (name) {
  case 'bash':
  case 'powershell': {
   const code = String(args.command || '');
   const powershell = name === 'powershell';
   return { kind: 'command', title: TEXT.command, effect: powershell ? shellEffect(code) : posixEffect(code), badge: true, places: powershell ? shellTargets(code, cwd) : posixTargets(code, cwd), code, reveal: 'command' };
  }
  case 'write': return { kind: 'file', title: TEXT.write, effect: 'change', places: [file], added: String(args.content || ''), reveal: 'content' };
  case 'edit': {
   const edits = Array.isArray(args.edits) ? args.edits : [];
   return { kind: 'file', title: TEXT.edit, effect: 'change', places: [file], removed: edits.map(e => String(e?.oldText ?? '')).join('\n'), added: edits.map(e => String(e?.newText ?? '')).join('\n'), reveal: 'changes' };
  }
  case 'read': return { kind: 'file', title: TEXT.read, effect: 'read', places: [file] };
  case 'ls':
  case 'grep':
  case 'find': return { kind: 'file', title: TEXT.list, effect: 'read', places: [place(args.path || '.', cwd)] };
  default: {
   let code = '';
   try { code = JSON.stringify(args); } catch {}
   return { kind: 'command', title: name, effect: READS.has(name) ? 'read' : 'run', places: [], code, reveal: 'command' };
  }
 }
}
