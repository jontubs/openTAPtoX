param(
  [Parameter(Mandatory = $true)]
  [string]$BinaryPath,
  [Parameter(Mandatory = $true)]
  [string]$EspIp,
  [string]$HostIp = "",
  [int]$EspOtaPort = 3232,
  [int]$HostOtaPort = 3233
)

$ErrorActionPreference = "Stop"

function Resolve-HostIp([System.Net.IPAddress]$TargetIp) {
  $probe = [System.Net.Sockets.UdpClient]::new()
  try {
    $probe.Connect($TargetIp, 3232)
    return ([System.Net.IPEndPoint]$probe.Client.LocalEndPoint).Address.IPAddressToString
  } finally {
    $probe.Dispose()
  }
}

$targetIp = [System.Net.IPAddress]::Parse($EspIp)
if ([string]::IsNullOrWhiteSpace($HostIp)) {
  $HostIp = Resolve-HostIp $targetIp
}
$localIp = [System.Net.IPAddress]::Parse($HostIp)
$image = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $BinaryPath))
$md5 = [System.Security.Cryptography.MD5]::Create()
$imageMd5 = ([System.BitConverter]::ToString($md5.ComputeHash($image))).Replace("-", "").ToLowerInvariant()

$listener = [System.Net.Sockets.TcpListener]::new($localIp, $HostOtaPort)
$udp = $null
$client = $null
$stream = $null

try {
  $listener.Start()
  $udp = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new($localIp, 0))
  $udp.Client.ReceiveTimeout = 1500
  $invitation = [System.Text.Encoding]::ASCII.GetBytes("0 $HostOtaPort $($image.Length) $imageMd5`n")
  $accepted = $false

  for ($attempt = 1; $attempt -le 5 -and -not $accepted; ++$attempt) {
    [void]$udp.Send($invitation, $invitation.Length,
                    [System.Net.IPEndPoint]::new($targetIp, $EspOtaPort))
    try {
      $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
      $reply = [System.Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
      $accepted = $reply -eq "OK"
    } catch [System.Net.Sockets.SocketException] {
      Start-Sleep -Milliseconds 400
    }
  }

  if (-not $accepted) {
    throw "ESP did not accept the OTA invitation."
  }

  $accept = $listener.AcceptTcpClientAsync()
  if (-not $accept.Wait(15000)) {
    throw "ESP did not open the OTA return connection to $HostIp`:$HostOtaPort."
  }

  $client = $accept.Result
  $stream = $client.GetStream()
  $stream.ReadTimeout = 12000
  $stream.WriteTimeout = 12000
  $offset = 0
  while ($offset -lt $image.Length) {
    $count = [Math]::Min(1024, $image.Length - $offset)
    $stream.Write($image, $offset, $count)
    $offset += $count
    $ack = New-Object byte[] 16
    [void]$stream.Read($ack, 0, $ack.Length)
  }

  $resultBuffer = New-Object byte[] 32
  $resultLength = $stream.Read($resultBuffer, 0, $resultBuffer.Length)
  $result = [System.Text.Encoding]::ASCII.GetString($resultBuffer, 0, $resultLength)
  if ($result -notmatch "OK") {
    throw "ESP did not confirm the firmware update: $result"
  }

  Write-Output "OTA upload completed: $offset bytes to $EspIp via $HostIp`:$HostOtaPort"
} finally {
  if ($stream) { $stream.Dispose() }
  if ($client) { $client.Dispose() }
  if ($udp) { $udp.Dispose() }
  $listener.Stop()
}
