import javax.swing.*;
import java.awt.*;
import java.io.*;
import java.net.*;
import java.util.*;

public class Client extends JFrame {
  private JTextField tfSpeed = new JTextField("--");
  private JTextField tfBatt  = new JTextField("--");
  private JTextField tfSta   = new JTextField("--");
  private JTextField tfDir   = new JTextField("--");
  private JLabel status = new JLabel("Conectando...");

  private final String host; private final int port; private final String adminToken;
  private Socket socket; private BufferedReader in; private PrintWriter out;

  public Client(String host, int port, String adminToken){
    super("Metro Telemetría (Java)");
    this.host=host; this.port=port; this.adminToken=adminToken;
    buildUI(); connect();
    setSize(460,230); setLocationRelativeTo(null); setVisible(true);
  }

  private void buildUI(){
    setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
    setLayout(new BorderLayout(8,8));
    JPanel grid = new JPanel(new GridLayout(4,2,8,8));
    grid.add(new JLabel("Velocidad (km/h):")); tfSpeed.setEditable(false); grid.add(tfSpeed);
    grid.add(new JLabel("Batería (%):"));      tfBatt.setEditable(false);  grid.add(tfBatt);
    grid.add(new JLabel("Estación:"));         tfSta.setEditable(false);   grid.add(tfSta);
    grid.add(new JLabel("Dirección:"));        tfDir.setEditable(false);   grid.add(tfDir);
    add(grid, BorderLayout.CENTER);

    JPanel btns = new JPanel(new FlowLayout());
    JButton bUp=new JButton("SPEED_UP"), bDown=new JButton("SLOW_DOWN"),
            bStop=new JButton("STOPNOW"), bStart=new JButton("STARTNOW");
    btns.add(bUp); btns.add(bDown); btns.add(bStop); btns.add(bStart);
    add(btns, BorderLayout.SOUTH); add(status, BorderLayout.NORTH);

    bUp.addActionListener(e -> send("CMD SPEED_UP"));
    bDown.addActionListener(e -> send("CMD SLOW_DOWN"));
    bStop.addActionListener(e -> send("CMD STOPNOW"));
    bStart.addActionListener(e -> send("CMD STARTNOW"));
  }

  private void connect(){
    new Thread(() -> {
      while (true){
        try{
          socket = new Socket();
          socket.connect(new InetSocketAddress(host, port), 4000);
          in  = new BufferedReader(new InputStreamReader(socket.getInputStream(),"UTF-8"));
          out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(),"UTF-8"), true);
          setStatus("Conectado a "+host+":"+port);
          if (adminToken!=null && !adminToken.isEmpty()) send("ADMIN token="+adminToken);
          new Thread(this::recvLoop,"recv").start();
          break;
        }catch(Exception ex){
          setStatus("Reintentando... "+ex.getMessage());
          try{ Thread.sleep(1500);}catch(Exception ignore){}
        }
      }
    },"connector").start();
  }

  private void recvLoop(){
    try{
      String line;
      while((line=in.readLine())!=null) handleLine(line.trim());
      setStatus("Conexión cerrada por servidor");
      reconnect();
    }catch(Exception ex){
      setStatus("Error: "+ex.getMessage()); reconnect();
    }
  }

  private void reconnect(){
    closeQuietly(); try{ Thread.sleep(1500);}catch(Exception ignore){}
    connect();
  }

  private void send(String s){
    try{ out.println(s); setStatus("Enviado: "+s);}catch(Exception ex){ setStatus("Error envío: "+ex.getMessage()); }
  }

  private void handleLine(String line){
    if(line.isEmpty()) return;
    if(line.equals("ACK")){ setStatus("ACK"); return; }
    if(line.startsWith("NACK")){ setStatus(line); return; }

    String[] parts = line.split("\\s+");
    String kind = parts[0];
    Map<String,String> kv = new HashMap<>();
    for(int i=1;i<parts.length;i++){
      int j=parts[i].indexOf('=');
      if(j>0) kv.put(parts[i].substring(0,j), parts[i].substring(j+1));
    }
    if ("TELEMETRY".equals(kind)){
      tfSpeed.setText(kv.getOrDefault("speed","--"));
      tfBatt.setText(kv.getOrDefault("battery","--"));
      tfSta.setText(kv.getOrDefault("station","--"));
      tfDir.setText(kv.getOrDefault("direction","--"));
      setStatus("Telemetría actualizada");
    } else if ("EVENT".equals(kind)){
      setStatus("Evento: "+line);
    } else {
      setStatus("Otro: "+line);
    }
  }

  private void setStatus(String s){ SwingUtilities.invokeLater(() -> status.setText(s)); }
  private void closeQuietly(){
    try{ if(in!=null) in.close(); }catch(Exception ignore){}
    try{ if(out!=null) out.close(); }catch(Exception ignore){}
    try{ if(socket!=null) socket.close(); }catch(Exception ignore){}
  }

  public static void main(String[] args){
    String host = (args.length>=1)? args[0] : "127.0.0.1";
    int port    = (args.length>=2)? Integer.parseInt(args[1]) : 5000;
    String token= (args.length>=3)? args[2] : null;
    SwingUtilities.invokeLater(() -> new Client(host, port, token));
  }
}
