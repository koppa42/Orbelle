# Optional STK Engine benchmark. Creates its own headless scenario; never attaches
# to an existing user application. All times are propagation-only milliseconds.
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$GravityFile,
    [string]$Inputs = (Join-Path $PSScriptRoot '../../tests/reference/propagators/hpop_performance.json'),
    [string[]]$Cases = @('scenario4','leo','gnss','geo','eccentric','scenario4_week'),
    [ValidateSet('point_mass','gravity21','full','tides','third_body','srp','drag')][string[]]$Forces = @('full'),
    [ValidateRange(1,100)][int]$Runs = 3,
    [string]$EopFile,
    [ValidateRange(1e-16,1e-6)][double]$ErrorTolerance = 1e-13,
    [ValidateRange(1,86400)][double]$MaxStep = 86400,
    [ValidateRange(0,86400)][double]$FixedStep = 0,
    [switch]$NoShadow,
    [int]$StkMajorVersion = 11
)
$ErrorActionPreference = 'Stop'
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
$inputsFile = [System.IO.Path]::GetFullPath($Inputs)
$fixture = Get-Content -LiteralPath $inputsFile -Raw | ConvertFrom-Json
if ($fixture.schema_version -ne 1) { throw 'Unsupported input schema' }
foreach ($name in $Cases) {
    if ($null -eq $fixture.cases.PSObject.Properties[$name]) { throw "Unknown case: $name" }
}
$engine = $null
$root = $null
$scenarioCreated = $false
try {
    $engine = New-Object -ComObject ("STKX{0}.Application" -f $StkMajorVersion)
    $engine.NoGraphics = $true
    $root = New-Object -ComObject ("AgStkObjects{0}.AgStkObjectRoot" -f $StkMajorVersion)
    $version = ($root.ExecuteCommand('GetSTKVersion /')).Item(0)
    $root.NewScenario('OrbelleHpopPerformance')
    $scenarioCreated = $true
    if ($EopFile) {
        $root.CurrentScenario.EarthData.EOPFilename = [IO.Path]::GetFullPath($EopFile)
        $root.CurrentScenario.EarthData.ReloadEOP()
    }
    Write-Output ('Loaded EOP: ' + $root.CurrentScenario.EarthData.EOPFilename + ' from ' + $root.CurrentScenario.EarthData.EOPStartTime + ' to ' + $root.CurrentScenario.EarthData.EOPStopTime)
    $root.UnitPreferences.SetCurrentUnit('DistanceUnit', 'm')
    $root.UnitPreferences.SetCurrentUnit('TimeUnit', 'sec')
    $root.UnitPreferences.SetCurrentUnit('DateFormat', 'UTCG')
    $utc = $fixture.epoch_utc
    $date = [datetime]::new($utc[0],$utc[1],$utc[2],$utc[3],$utc[4],$utc[5])
    $start = $date.ToString('dd MMM yyyy HH:mm:ss.fff',[cultureinfo]::InvariantCulture)
    $gravity = [System.IO.Path]::GetFullPath($GravityFile)
    $report = [ordered]@{version=$version; inputs_sha256=(Get-FileHash -LiteralPath $inputsFile -Algorithm SHA256).Hash; output_step_s=$fixture.output_step_s; gravity_sha256=(Get-FileHash -LiteralPath $gravity -Algorithm SHA256).Hash; cases=@()}
    $earthData = $root.CurrentScenario.EarthData
    $report.eop = [ordered]@{
        file = $earthData.EOPFilename
        first = $earthData.EOPStartTime
        last = $earthData.EOPStopTime
        sha256 = if ($EopFile) { (Get-FileHash -LiteralPath $EopFile -Algorithm SHA256).Hash } else { $null }
    }
    $report.integrator = [ordered]@{
        method = 'RKF78'; relative_tolerance = $ErrorTolerance
        min_step_s = 1; max_step_s = $MaxStep; fixed_step_s = $FixedStep
        report_on_fixed_step = $true; no_shadow = $NoShadow.IsPresent
    }
    foreach ($name in $Cases) {
        $initial = $fixture.cases.$name
        $stop = $date.AddSeconds($initial.duration_s).ToString('dd MMM yyyy HH:mm:ss.fff',[cultureinfo]::InvariantCulture)
        $root.CurrentScenario.SetTimePeriod($start, $stop)
        foreach ($forceName in $Forces) {
            $sat = $root.CurrentScenario.Children.New(18, ($name+'_'+$forceName))
            $sat.SetPropagatorType(0)
            $prop = $sat.Propagator
            $degree = if ($forceName -eq 'point_mass') { 0 } else { 21 }
            $force = $prop.ForceModel
            $force.CentralBodyGravity.File = $gravity
            $force.CentralBodyGravity.SetMaximumDegreeAndOrder($degree,$degree)
            $force.CentralBodyGravity.UseSecularVariations = $false
            $force.CentralBodyGravity.UseSolidTides = $false
            $force.CentralBodyGravity.UseOceanTides = $false
            $force.ThirdBodyGravity.RemoveAll()
            $force.Drag.Use = $false
            $force.SolarRadiationPressure.Use = $false
            $force.MoreOptions.Static.IncRelativisticAcc = $false
            $force.MoreOptions.RadiationPressure.IncludeAlbedo = $false
            $force.MoreOptions.RadiationPressure.IncludeThermal = $false
            $force.MoreOptions.Static.SatelliteMass = 1000
            $integrator = $prop.Integrator
            $integrator.IntegrationModel = 3
            $integrator.UseVOP = $false
            $integrator.TimeRegularization.Use = $false
            $integrator.StepSizeControl.Method = 1
            $integrator.StepSizeControl.ErrorTolerance = $ErrorTolerance
            $integrator.StepSizeControl.MinStepSize = 1
            $integrator.StepSizeControl.MaxStepSize = $MaxStep
            if ($FixedStep -gt 0) { $integrator.StepSizeControl.Method = 0 }
            $integrator.DoNotPropagateBelowAlt = 10000
            $integrator.ReportEphemOnFixedTimeStep = $true
            $integrator.Interpolation.Method = 1
            $integrator.Interpolation.Order = 7
            if ($forceName -ne 'point_mass' -and $forceName -ne 'gravity21') {
                $force.CentralBodyGravity.UseSolidTides = $true
                $force.CentralBodyGravity.SolidTideType = 1
                $force.MoreOptions.SolidTides.IncTimeDepSolidTides = $false
                $force.MoreOptions.SolidTides.TruncateSolidTides = $true
                if ($forceName -in @('full','third_body')) {
                    $sun = $force.ThirdBodyGravity.AddThirdBody('Sun')
                    $sun.Source = 3
                    $sun.GravityValue = 1.327122e20
                    $moon = $force.ThirdBodyGravity.AddThirdBody('Moon')
                    $moon.Source = 3
                    $moon.GravityValue = 4.902800305555e12
                }
                $force.Drag.Use = $true
                $force.Drag.SetDragModelType(0)
                $force.Drag.DragModel.Cd = 2.2
                $force.Drag.DragModel.AreaMassRatio = .02
                $force.Drag.AtmosphericDensityModel = 4
                $force.Drag.SetSolarFluxGeoMagType(0)
                $force.Drag.SolarFluxGeoMag.DailyF107 = 150
                $force.Drag.SolarFluxGeoMag.AverageF107 = 150
                $force.Drag.SolarFluxGeoMag.GeomagneticIndex = 3
                $force.MoreOptions.Drag.UseApparentSunPosition = $true
                $force.MoreOptions.Drag.UseApproxAlt = $true
                $force.SolarRadiationPressure.Use = $true
                $force.SolarRadiationPressure.SRPModel.SetModelType(6)
                $force.SolarRadiationPressure.SRPModel.Model.Cr = 1
                $force.SolarRadiationPressure.SRPModel.Model.AreaMassRatio = .02
                $force.SolarRadiationPressure.ShadowModel = if ($NoShadow) { 2 } else { 1 }
                $force.SolarRadiationPressure.UseBoundaryMitigation = $false
                $force.MoreOptions.SolarRadiationPressure.MethodToComputeSunPosition = 1
                Write-Output ('STK eclipse atmosphere height (m): ' + $force.MoreOptions.SolarRadiationPressure.AtmosAltOfEarthShapeForEclipse)
                $force.EclipsingBodies.RemoveAllEclipsingBodies()
                $force.EclipsingBodies.AssignEclipsingBody('Earth')
                $force.EclipsingBodies.AssignEclipsingBody('Moon')
                # Setting UseSolidTides=true again resets STK to Full tides.
                # Configure while enabled, then only disable unwanted forces.
                if ($forceName -notin @('full','tides')) { $force.CentralBodyGravity.UseSolidTides = $false }
                $force.Drag.Use = $forceName -in @('full','drag')
                $force.SolarRadiationPressure.Use = $forceName -in @('full','srp')
            }
            $prop.EphemerisInterval.SetExplicitInterval($start, $stop)
            $prop.Step = if ($FixedStep -gt 0) { $FixedStep } else { $fixture.output_step_s }
            $prop.InitialState.OrbitEpoch.SetExplicitTime($start)
            $r = $initial.position_m
            $v = $initial.velocity_mps
            $prop.InitialState.Representation.AssignCartesian(11, $r[0],$r[1],$r[2],$v[0],$v[1],$v[2])
            $times = @()
            for ($run=0; $run -lt $Runs; ++$run) {
                $timer = [System.Diagnostics.Stopwatch]::StartNew()
                $prop.Propagate()
                $timer.Stop()
                $times += $timer.Elapsed.TotalMilliseconds
                Write-Output ("{0}/{1} run {2}: {3:N3} ms" -f $name,$forceName,($run+1),$timer.Elapsed.TotalMilliseconds)
            }
            $positions = $sat.DataProviders.Item('Cartesian Position').Group.Item('ICRF').Exec($start,$stop,$fixture.output_step_s)
            $velocities = $sat.DataProviders.Item('Cartesian Velocity').Group.Item('ICRF').Exec($start,$stop,$fixture.output_step_s)
            $samples = [ordered]@{frame='ICRF';units='m,m/s';time=$positions.DataSets.GetDataSetByName('Time').GetValues();position=@();velocity=@()}
            foreach($component in @('x','y','z')) { $samples.position += ,$positions.DataSets.GetDataSetByName($component).GetValues(); $samples.velocity += ,$velocities.DataSets.GetDataSetByName($component).GetValues() }
            $samples | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output ($name+'-'+$forceName+'-samples.json')) -Encoding utf8
            $report.cases += [ordered]@{name=$name; forces=$forceName; propagation_ms=$times; duration_s=$initial.duration_s; initial_gcrf=@{epoch_tai_ns=$fixture.epoch_tai_ns; position_m=$initial.position_m; velocity_mps=$initial.velocity_mps}}
            $report | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $output 'profile.json') -Encoding utf8
        }
    }
    $root.SaveScenarioAs((Join-Path $output 'OrbelleHpopPerformance.sc'))
} finally {
    if ($null -ne $root) {
        try { if ($scenarioCreated) { $root.CloseScenario() } }
        finally { [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($root) }
    }
    if ($null -ne $engine) { [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine) }
}
