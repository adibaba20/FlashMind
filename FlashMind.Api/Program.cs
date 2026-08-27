using System.Diagnostics;
using FlashMind.Api;

var builder = WebApplication.CreateBuilder(args);

// Allow React frontend to call this API
builder.Services.AddCors(options =>
{
    options.AddPolicy("Frontend", policy =>
    {
        policy
            .AllowAnyOrigin()
            .AllowAnyHeader()
            .AllowAnyMethod();
    });
});

var app = builder.Build();

// Enable CORS
app.UseCors("Frontend");


// ============================================================
// GET /api/simulator/run
// Runs the C SSD simulator
// ============================================================

app.MapGet("/api/simulator/run", () =>
{
    var projectRoot = Directory.GetParent(AppContext.BaseDirectory)!
        .Parent!
        .Parent!
        .Parent!
        .Parent!
        .FullName;

    var simulatorPath = Path.Combine(
        projectRoot,
        "final_version",
        "flashmind_sim.exe"
    );

    if (!File.Exists(simulatorPath))
    {
        return Results.NotFound(new
        {
            error = "Simulator executable not found",
            path = simulatorPath
        });
    }

    var startInfo = new ProcessStartInfo
    {
        FileName = simulatorPath,
        WorkingDirectory = Path.GetDirectoryName(simulatorPath)!,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true
    };

    using var process = new Process();

    process.StartInfo = startInfo;

    process.Start();

    string output = process.StandardOutput.ReadToEnd();
    string error = process.StandardError.ReadToEnd();

    process.WaitForExit();

    var response = new SimulatorResponse
    {
        ExitCode = process.ExitCode,
        Output = output,
        Error = error
    };

    return Results.Ok(response);
});


// ============================================================
// GET /api/telemetry
// Reads telemetry.csv and returns JSON
// ============================================================

app.MapGet("/api/telemetry", () =>
{
    var telemetryPath = Path.GetFullPath(
        Path.Combine(
            app.Environment.ContentRootPath,
            "..",
            "final_version",
            "data",
            "raw",
            "telemetry.csv"
        )
    );

    if (!File.Exists(telemetryPath))
    {
        return Results.NotFound(new
        {
            error = "telemetry.csv not found",
            path = telemetryPath
        });
    }

    var lines = File.ReadAllLines(telemetryPath);

    if (lines.Length == 0)
    {
        return Results.Ok(Array.Empty<object>());
    }

    var headers = lines[0].Split(',');

    var rows = lines
        .Skip(1)
        .Where(line => !string.IsNullOrWhiteSpace(line))
        .Select(line =>
        {
            var values = line.Split(',');

            return headers
                .Select((header, index) => new
                {
                    header,
                    value = index < values.Length
                        ? values[index]
                        : ""
                })
                .ToDictionary(
                    x => x.header,
                    x => x.value
                );
        })
        .ToList();

    return Results.Ok(rows);
});

// ============================================================
// GET /api/ml/predictions
// Runs the Python ML prediction pipeline
// ============================================================

app.MapGet("/api/ml/predictions", () =>
{
    var projectRoot = Directory.GetParent(AppContext.BaseDirectory)!
        .Parent!
        .Parent!
        .Parent!
        .Parent!
        .FullName;

    var pythonScript = Path.Combine(
        projectRoot,
        "final_version",
        "src",
        "predict.py"
    );

    if (!File.Exists(pythonScript))
    {
        return Results.NotFound(new
        {
            error = "Prediction script not found",
            path = pythonScript
        });
    }

    var startInfo = new ProcessStartInfo
    {
        FileName = "python",
        Arguments = $"\"{pythonScript}\"",
        WorkingDirectory = Path.GetDirectoryName(pythonScript)!,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true
    };

    using var process = new Process();

    process.StartInfo = startInfo;

    process.Start();

    string output = process.StandardOutput.ReadToEnd();
    string error = process.StandardError.ReadToEnd();

    process.WaitForExit();

    if (process.ExitCode != 0)
    {
        return Results.Problem(
            detail: error,
            statusCode: 500
        );
    }

    return Results.Content(
        output,
        "application/json"
    );
});
app.Run();