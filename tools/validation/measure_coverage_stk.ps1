# Run actual STK coverage/FOM computations in a new, headless Engine instance.
# The shared scientific ephemeris removes propagation/EOP differences only;
# no Orbelle access interval or coverage result is supplied to STK.
param(
    [Parameter(Mandatory=$true)][string]$InputDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [int]$StkMajorVersion = 11,
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
$inputRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputRoot) { throw "Output already exists: $outputRoot" }
[void](New-Item -ItemType Directory -Path $outputRoot)
$inputFile = Join-Path $inputRoot 'orbelle.json'
$inputData = Get-Content -LiteralPath $inputFile -Raw | ConvertFrom-Json
if ($inputData.schema_version -ne 1) { throw 'Unsupported input schema' }
$config = $inputData.config

function Export-DataSets($result) {
    $values = [ordered]@{}
    for ($i = 0; $i -lt $result.DataSets.Count; ++$i) {
        $dataset = $result.DataSets.Item($i)
        $values[$dataset.ElementName] = @($dataset.GetValues())
    }
    return $values
}

function Save-Report($report) {
    $report | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $outputRoot 'stk.json') -Encoding utf8
}

$engine=$null
$root=$null
$scenarioCreated=$false
try {
    $engine = New-Object -ComObject ("STKX{0}.Application" -f $StkMajorVersion)
    $engine.NoGraphics = $true
    $root = New-Object -ComObject ("AgStkObjects{0}.AgStkObjectRoot" -f $StkMajorVersion)
    $version = ($root.ExecuteCommand('GetSTKVersion /')).Item(0)
    $root.NewScenario('OrbelleCoverageComparison')
    $scenarioCreated = $true
    $root.UnitPreferences.SetCurrentUnit('DateFormat','UTCG')
    $root.UnitPreferences.SetCurrentUnit('DistanceUnit','m')
    $root.UnitPreferences.SetCurrentUnit('TimeUnit','sec')
    $root.UnitPreferences.SetCurrentUnit('AngleUnit','deg')
    $root.UnitPreferences.SetCurrentUnit('LatitudeUnit','deg')
    $root.UnitPreferences.SetCurrentUnit('LongitudeUnit','deg')
    $root.CurrentScenario.Epoch = $config.start_utc
    $root.UnitPreferences.SetCurrentUnit('DateFormat','EpSec')
    $root.CurrentScenario.SetTimePeriod(0,$config.duration_s)
    $earth=$root.CentralBodies.Item('Earth')
    $report = [ordered]@{schema_version=1; version=$version;
        orbelle_input_sha256=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash;
        earth=[ordered]@{a_m=$earth.Ellipsoid.A; b_m=$earth.Ellipsoid.B; c_m=$earth.Ellipsoid.C;
            gravitational_parameter_m3_s2=$earth.GravitationalParameter};
        settings=[ordered]@{light_time=$false; terrain=$false;
            search_step_s=$inputData.search_step_s; boundary_tolerance_s=$inputData.boundary_tolerance_s;
            interpolation='Lagrange, 8 points, Fixed Cartesian position/velocity';
            pointing='Geocentric nadir; NadirECIVel Offset 0; fixed sensor az=0/el=90';
            area_policy='Native STK point areas exported separately; matched areas used by comparison'};
        satellites=@(); cases=@()}
    foreach ($item in $inputData.satellites) {
        $ephemeris = Join-Path $inputRoot $item.ephemeris_file
        if ((Get-FileHash -LiteralPath $ephemeris -Algorithm SHA256).Hash -ne $item.sha256) {
            throw 'Shared ephemeris hash mismatch'
        }
        $satellite = $root.CurrentScenario.Children.New(18,$item.name)
        $satellite.SetPropagatorType(6)
        $satellite.Propagator.Filename = $ephemeris
        $satellite.Propagator.Propagate()
        # Nadir alignment is explicitly geocentric; cone clock angle is immaterial.
        [void]$root.ExecuteCommand("SetAttitude */Satellite/$($item.name) Profile NadirECIVel Offset 0")
        $sensor = $satellite.Children.New(20,'Cone')
        [void]$sensor.CommonTasks.SetPatternSimpleConic($config.cone_half_angle_deg,0.1)
        [void]$sensor.CommonTasks.SetPointingFixedAzEl(0,90,0)
        $checks = @()
        foreach ($sample in $item.checks) {
            $position = $satellite.DataProviders.Item('Cartesian Position').Group.Item('Fixed').ExecSingle($sample.time_s)
            $velocity = $satellite.DataProviders.Item('Cartesian Velocity').Group.Item('Fixed').ExecSingle($sample.time_s)
            $r=@(); $v=@()
            foreach($axis in @('x','y','z')) {
                $r += [double]$position.DataSets.GetDataSetByName($axis).GetValues()[0]
                $v += [double]$velocity.DataSets.GetDataSetByName($axis).GetValues()[0]
            }
            $checks += [ordered]@{time_s=$sample.time_s; position_m=$r; velocity_mps=$v}
        }
        $report.satellites += [ordered]@{name=$item.name; checks=$checks}
    }
    $seed = $root.CurrentScenario.Children.New(8,'GridConstraints')
    $seed.UseTerrain = $false
    $seed.Position.AssignGeodetic(0,0,0)
    $elevation = $seed.AccessConstraints.AddConstraint(14)
    $elevation.EnableMin=$true
    $elevation.Min=$config.minimum_elevation_deg
    $elevation.EnableMax=$false
    foreach ($case in $inputData.cases) {
        if ($Cases.Count -gt 0 -and $case.name -notin $Cases) { continue }
        Write-Output ("STK computing " + $case.name)
        $duration = if ($null -ne $case.duration_s) { $case.duration_s } else { $config.duration_s }
        $coverage=$root.CurrentScenario.Children.New(7,$case.name)
        $coverage.Advanced.AutoRecompute=$false
        $coverage.Advanced.EnableLightTimeDelay=$false
        $coverage.Advanced.RegionAccessAcceleration=1
        $coverage.Interval.AnalysisInterval.SetExplicitInterval(0,$duration)
        $coverage.Advanced.Sampling.SetType(1)
        $coverage.Advanced.Sampling.Strategy.FixedTimeStep=$inputData.search_step_s
        $coverage.Advanced.Sampling.Strategy.TimeBound=0
        $coverage.Advanced.EventDetection.SetType(1)
        $coverage.Advanced.EventDetection.Strategy.TimeConvergence=$inputData.boundary_tolerance_s
        $coverage.Advanced.EventDetection.Strategy.RelValueConvergence=1e-12
        $coverage.Advanced.EventDetection.Strategy.AbsValueConvergence=1e-12
        $coverage.Grid.ResolutionType=2
        $coverage.Grid.Resolution.LatLon=$config.resolution_deg
        if ($null -ne $case.region.bounds_deg) {
            $coverage.Grid.BoundsType=6
            $coverage.Grid.Bounds.MinLatitude=$case.region.bounds_deg[0]
            $coverage.Grid.Bounds.MaxLatitude=$case.region.bounds_deg[1]
            $coverage.Grid.Bounds.MinLongitude=$case.region.bounds_deg[2]
            $coverage.Grid.Bounds.MaxLongitude=$case.region.bounds_deg[3]
        } else {
            $area=$root.CurrentScenario.Children.New(2,'Region_'+$case.name)
            $area.AreaType=1
            $area.AreaTypeData.RemoveAll()
            foreach($vertex in $case.region.vertices_deg) { [void]$area.AreaTypeData.Add($vertex[1],$vertex[0]) }
            $coverage.Grid.BoundsType=0
            [void]$coverage.Grid.Bounds.AreaTargets.Add('AreaTarget/'+$area.InstanceName)
        }
        $points=Join-Path $inputRoot $case.point_file
        if ((Get-FileHash -LiteralPath $points -Algorithm SHA256).Hash -ne $case.point_sha256) {
            throw 'Shared point file hash mismatch'
        }
        $coverage.PointDefinition.PointLocationMethod=1
        [void]$coverage.PointDefinition.PointFileList.Add($points)
        $coverage.PointDefinition.AltitudeMethod=1
        $coverage.PointDefinition.Altitude=0
        $coverage.PointDefinition.GridClass=1
        $coverage.PointDefinition.UseGridSeed=$true
        $coverage.PointDefinition.SeedInstance='Facility/GridConstraints'
        foreach ($satellite in $inputData.satellites) {
            $asset='Satellite/'+$satellite.name
            if ($case.mode -eq 'cones') {$asset += '/Sensor/Cone'}
            [void]$coverage.AssetList.Add($asset)
        }
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $coverage.ComputeAccesses()
        $timer.Stop()
        $row=[ordered]@{name=$case.name; calculation_ms=$timer.Elapsed.TotalMilliseconds;
            grid=(Export-DataSets ($coverage.DataProviders.Item('Grid Point Locations').Exec()));
            percent_coverage=(Export-DataSets ($coverage.DataProviders.Item('Percent Coverage').Exec(0,$duration,30)));
            points=@(); foms=[ordered]@{}}
        foreach ($point in $case.points) {
            $coverage.GridInspector.SelectPoint($point.lat_deg,$point.lon_deg)
            $data=$coverage.DataProviders.Item('Selected Point Coverage').Exec(0,$duration)
            $row.points += [ordered]@{lat_deg=$point.lat_deg; lon_deg=$point.lon_deg; data=(Export-DataSets $data)}
        }
        foreach($definition in @(
            @{name='covered_s';type=3;compute=16},
            @{name='max_gap_s';type=10;compute=1;ends=1},
            @{name='mean_gap_s';type=10;compute=0;ends=1},
            @{name='internal_max_gap_s';type=10;compute=1;ends=0},
            @{name='internal_mean_gap_s';type=10;compute=0;ends=0}
        )) {
            $fom=$coverage.Children.New(25,$definition.name)
            $fom.SetDefinitionType($definition.type)
            $fom.Definition.SetComputeType($definition.compute)
            if($definition.ContainsKey('ends')) { $fom.Definition.EndGapOption=$definition.ends }
            $row.foms[$definition.name] = Export-DataSets ($fom.DataProviders.Item('Value By Point').Exec())
        }
        $report.cases += $row
        Save-Report $report
        Write-Output ("Completed {0}: {1} grid points, {2:N1} ms" -f $case.name,$row.grid.Latitude.Count,$timer.Elapsed.TotalMilliseconds)
    }
    $root.SaveScenarioAs((Join-Path $outputRoot 'comparison.sc'))
    Save-Report $report
} finally {
    if($scenarioCreated) { $root.CloseScenario() }
    if($null -ne $root) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($root) }
    if($null -ne $engine) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine) }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
