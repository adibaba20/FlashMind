namespace FlashMind.Api;

public class SimulatorResponse
{
    public int ExitCode { get; set; }
    public string Output { get; set; } = "";
    public string Error { get; set; } = "";
}