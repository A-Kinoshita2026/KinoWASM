using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;

// Windows x64 example. Use only one instance: the native bridge owns one session.
public sealed class KinoCooperativeExample : MonoBehaviour
{
	private const string native_library = "kino_cooperative";
	private bool session_open;
	private bool running;

	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern int kino_example_open([In] byte[] wasm_data, uint wasm_size);
	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern int kino_example_tick();
	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern int kino_example_progress();
	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern int kino_example_result();
	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern uint kino_example_error();
	[DllImport(native_library, CallingConvention = CallingConvention.Cdecl)]
	private static extern void kino_example_close();

	private void OnEnable()
	{
		try
		{
			byte[] wasm_data = File.ReadAllBytes(Path.Combine(Application.streamingAssetsPath, "cooperative.wasm"));
			if (kino_example_open(wasm_data, checked((uint)wasm_data.Length)) != 0)
			{
				Debug.LogError($"KinoWASM load failed: {kino_example_error()}", this);
				return;
			}
			session_open = true;
			running = true;
		}
		catch (Exception error)
		{
			Debug.LogException(error, this);
		}
	}

	private void Update()
	{
		if (!running)
			return;

		// One invocation/resumption per frame. Do not loop while suspended.
		int status = kino_example_tick();
		Debug.Log($"KinoWASM progress: {kino_example_progress()}", this);
		if (status == 1)
			return;

		running = false;
		if (status == 0)
			Debug.Log($"KinoWASM completed: {kino_example_result()}", this);
		else
			Debug.LogError($"KinoWASM failed: {kino_example_error()}", this);
	}

	private void OnDisable()
	{
		running = false;
		if (session_open)
			kino_example_close();

		session_open = false;
	}
}
